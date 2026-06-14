;; Keywords: scene scenegraph os ui render
;; Scene graph data, object transforms, and draw traversal support for UI projects.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.scene(
   scene_asset_name_from_gltf_path, first_gltf_in_dir, prefetch_gltf_asset_path, format_name_list,
   load_scene_mesh, load_scene_path, destroy_scene, unload_scene, scene_apply_fit,
   build_material_records, scene_apply_material_tweak, apply_gltf_animation,
   scene_mesh_num, scene_mesh_int, scene_mesh_bool, scene_model_baked_for_draw,
   scene_edit_scale, scene_has_edit_transform, scene_edit_transform_matrix_into,
   scene_active_model_matrix_into, scene_fit_transform_into,
   scene_drag_begin_state, scene_drag_pixel_scale, scene_drag_camera_basis, scene_drag_apply,
   scene_fast_draw, scene_fast_color_reuse_ready, scene_fast_reset,
   first_ready, mode_flags, scene_material_info,
   base_pair, scene_fallbacks, apply_overrides, scene_override_pair, normalize_pair,
   generated_plan, background_requested,
   proof_needs_generated, proof_fallbacks, skybox_fallback,
   upload_generated_texture
)

use std.core
use std.os.fs as osfs
use std.os.info
use std.os.path as ospath
use std.os.thread
use std.os.time
use std.parse.data.json
use std.core.str as str
use std.os.ui.render.dump as ui_profile
use std.os.ui.assets.catalog as asset_catalog
use std.parse.img as lib_img
use std.parse.3d.gltf as gltf
use std.core.common as common
use std.os.ui.render.viewer.term as terminal
use std.math
use std.math.float (is_nan, is_inf)
use std.os.ui.render.camera as ui_camera
use std.os.ui.render as render
use std.os.ui.render.atlas as atlas
use std.os.ui.render.matrix (
   mat4_identity, mat4_identity_into, mat4_translate_into, mat4_scale_into, mat4_mul_into,
   mat4_rotate_x_into, mat4_rotate_y_into, mat4_rotate_z_into
)

use std.os.ui.render.utils as render_utils
use std.os.ui.render.shared as render_shared

def _SCENE_LIGHT_MAX = render_shared.SCENE_LIGHT_MAX
def MAX_TEXTURES = render_shared.MAX_TEXTURES
def _VKR_OFF_TEX = render_shared.OFF_TEX
def _VKR_OFF_C = render_shared.OFF_C
def _VKR_OFF_X = render_shared.OFF_X
def _VKR_OFF_Y = render_shared.OFF_Y
def _VKR_OFF_Z = render_shared.OFF_Z
def _VKR_OFF_NX = render_shared.OFF_NX
def _VKR_OFF_NY = render_shared.OFF_NY
def _VKR_OFF_NZ = render_shared.OFF_NZ
def _VKR_OFF_TX = render_shared.OFF_TX
def _VKR_OFF_TY = render_shared.OFF_TY
def _VKR_OFF_TZ = render_shared.OFF_TZ
def _VKR_OFF_TW = render_shared.OFF_TW
def _MESH_GPU_LINES = 1
def _MESH_GPU_UNLIT = 2
def _MESH_GPU_NOCULL = 4
def _MESH_GPU_INDEXED = 8
def _MESH_GPU_POINTS = 16
def _SCENE_ENV_SENSITIVE_MASK = (
   16 | 32 | 64 | 128 | 256 | 512 |
   1024 | 2048 | 4096 | 8192 | 32768 | 65536
)

fn _tex(dict tex, str key) int { int(tex.get(key, -1)) }

fn upload_generated_texture(any im, int format=37, int filter=1, int wrap_s=10497, int wrap_t=33071, bool mip=true) int {
   "Uploads a generated RGBA environment image dict and returns a texture id."
   if !im || !is_dict(im) { return -1 }
   def w = int(im.get("width", 0))
   def h = int(im.get("height", 0))
   def data = im.get("data", 0)
   if !data || w <= 0 || h <= 0 { return -1 }
   render.texture_upload_image_ex(im, "generated-env", format, mip, false, filter, wrap_s, wrap_t, "generated-env:" + to_str(w) + "x" + to_str(h), false)
}

fn first_ready(any ids) int {
   "Returns the first non-negative texture id from `ids`."
   if !is_list(ids) { return -1 }
   mut i = 0
   while i < ids.len {
      def id = int(ids.get(i, -1))
      if id >= 0 { return id }
      i += 1
   }
   -1
}

fn mode_flags(
   bool pref_studio,
   bool pref_neutral,
   bool pref_reflect,
   bool pref_visible,
   bool pref_optical,
   bool batch_on,
   int gui_env_mode
) list {
   "Resolves scene environment mode booleans from preferences and UI override mode."
   mut studio_env = pref_studio
   mut neutral_env = pref_neutral
   mut compare_reflect_env = pref_reflect || pref_optical
   mut compare_visible_env = pref_visible
   mut optical_spec_env = pref_optical
   if !batch_on && gui_env_mode == 1 {
      studio_env = true
      neutral_env = false
      compare_reflect_env = true
      compare_visible_env = false
      optical_spec_env = false
   } elif !batch_on && gui_env_mode == 2 {
      studio_env = false
      neutral_env = true
      compare_reflect_env = false
      compare_visible_env = true
      optical_spec_env = false
   } elif !batch_on && gui_env_mode == 3 {
      studio_env = false
      neutral_env = false
      compare_reflect_env = false
      compare_visible_env = false
      optical_spec_env = true
   }
   [studio_env, neutral_env, compare_reflect_env, compare_visible_env, optical_spec_env]
}

fn scene_material_info(any scene_obj, bool studio_env, bool neutral_env) list {
   "Returns material feature/env sensitivity details for environment selection."
   def scene_ok = scene_obj != 0 && is_dict(scene_obj)
   def mat_mask = scene_ok ? int(scene_obj.get("material_feature_mask", 0)) : 0
   def env_sensitive = scene_ok && scene_obj.get("scene_env_sensitive_materials", false)
   def needs_reflect_spec = band(mat_mask, 16 | 32 | 64 | 1024 | 2048 | 4096) != 0
   def needs_optical_spec = band(mat_mask, 128 | 256 | 512 | 4096 | 8192 | 32768 | 65536) != 0
   [
      mat_mask, env_sensitive, needs_reflect_spec, needs_optical_spec,
      env_sensitive && !(studio_env || neutral_env),
   ]
}

fn base_pair(dict tex, bool studio_env, bool neutral_env, bool compare_reflect_env, bool compare_visible_env, bool optical_spec_env, bool feature_fallback_env) list {
   "Returns the base [environment, specular-environment] texture pair."
   mut env_tex = -1
   if studio_env && _tex(tex, "compare_env") >= 0 { env_tex = _tex(tex, "compare_env") }
   elif neutral_env && compare_visible_env && _tex(tex, "compare_visible_env") >= 0 { env_tex = _tex(tex, "compare_visible_env") }
   elif neutral_env && _tex(tex, "neutral_env") >= 0 { env_tex = _tex(tex, "neutral_env") }
   elif !feature_fallback_env && _tex(tex, "skybox") >= 0 { env_tex = _tex(tex, "skybox") }
   mut env_spec_tex = -1
   if studio_env {
      if compare_reflect_env {
         env_spec_tex = first_ready([_tex(tex, "compare_reflect_spec"), _tex(tex, "compare_env_spec"), _tex(tex, "compare_env"), _tex(tex, "skybox_spec"), _tex(tex, "skybox")])
      } else {
         env_spec_tex = first_ready([_tex(tex, "compare_env_spec"), _tex(tex, "compare_env"), _tex(tex, "skybox_spec"), _tex(tex, "skybox")])
      }
   } elif neutral_env {
      if compare_reflect_env && _tex(tex, "compare_reflect_spec") >= 0 { env_spec_tex = _tex(tex, "compare_reflect_spec") }
      elif optical_spec_env { env_spec_tex = first_ready([_tex(tex, "skybox_spec"), _tex(tex, "skybox")]) }
      else { env_spec_tex = first_ready([_tex(tex, "compare_env_spec"), _tex(tex, "compare_env")]) }
      if env_spec_tex < 0 {
         env_spec_tex = first_ready([_tex(tex, "neutral_env_spec"), _tex(tex, "neutral_env"), _tex(tex, "skybox_spec"), _tex(tex, "skybox")])
      }
   } elif !feature_fallback_env && _tex(tex, "skybox_spec") >= 0 { env_spec_tex = _tex(tex, "skybox_spec") }
   [env_tex, env_spec_tex]
}

fn scene_fallbacks(dict tex, int env_tex, int env_spec_tex, bool scene_env_sensitive_materials, bool scene_needs_reflect_spec, bool scene_needs_optical_spec, bool compare_visible_env) list {
   "Applies material-driven environment fallbacks."
   mut out_env_tex = env_tex
   mut out_env_spec_tex = env_spec_tex
   if out_env_tex < 0 && scene_env_sensitive_materials {
      if scene_needs_optical_spec && compare_visible_env && _tex(tex, "compare_visible_env") >= 0 { out_env_tex = _tex(tex, "compare_visible_env") }
      elif _tex(tex, "neutral_env") >= 0 { out_env_tex = _tex(tex, "neutral_env") }
      elif scene_needs_optical_spec && _tex(tex, "compare_env") >= 0 { out_env_tex = _tex(tex, "compare_env") }
      else { out_env_tex = first_ready([_tex(tex, "compare_env"), _tex(tex, "skybox")]) }
   }
   if out_env_spec_tex < 0 && scene_env_sensitive_materials {
      if scene_needs_optical_spec { out_env_spec_tex = first_ready([_tex(tex, "skybox_spec"), _tex(tex, "skybox")]) }
      if out_env_spec_tex < 0 && scene_needs_reflect_spec && _tex(tex, "compare_reflect_spec") >= 0 {
         out_env_spec_tex = _tex(tex, "compare_reflect_spec")
      }
      if out_env_spec_tex < 0 {
         out_env_spec_tex = first_ready([
               _tex(tex, "neutral_env_spec"), _tex(tex, "neutral_env"), _tex(tex, "compare_env_spec"),
               _tex(tex, "compare_env"), _tex(tex, "skybox_spec"), _tex(tex, "skybox"),
         ])
      }
   }
   [out_env_tex, out_env_spec_tex]
}

fn apply_overrides(dict tex, int env_tex, int env_spec_tex, bool batch_on, int gui_env_mode, bool black_visible, bool disable_env=false, bool disable_env_spec=false) list {
   "Applies environment UI overrides."
   mut out_env_tex = env_tex
   mut out_env_spec_tex = env_spec_tex
   if !batch_on && gui_env_mode == 3 {
      if _tex(tex, "skybox") >= 0 { out_env_tex = _tex(tex, "skybox") }
      out_env_spec_tex = first_ready([_tex(tex, "skybox_spec"), _tex(tex, "skybox")])
   } elif !batch_on && gui_env_mode == 4 {
      out_env_tex = -1
      out_env_spec_tex = -1
   }
   if black_visible || disable_env {
      out_env_tex = -1
      out_env_spec_tex = disable_env ? -1 : out_env_spec_tex
   } elif disable_env_spec { out_env_spec_tex = -1 }
   [out_env_tex, out_env_spec_tex]
}

fn scene_override_pair(
   dict tex,
   bool studio_env,
   bool neutral_env,
   bool compare_reflect_env,
   bool compare_visible_env,
   bool optical_spec_env,
   bool feature_fallback_env,
   bool scene_env_sensitive_materials,
   bool scene_needs_reflect_spec,
   bool scene_needs_optical_spec,
   bool batch_on,
   int gui_env_mode,
   bool black_visible,
   bool disable_env=false,
   bool disable_env_spec=false
) list {
   "Returns the full scene environment pair after fallbacks and UI overrides."
   def base = base_pair(tex, studio_env, neutral_env, compare_reflect_env, compare_visible_env, optical_spec_env, feature_fallback_env)
   def scene = scene_fallbacks(
      tex, int(base.get(0, -1)), int(base.get(1, -1)),
      scene_env_sensitive_materials, scene_needs_reflect_spec,
      scene_needs_optical_spec, compare_visible_env
   )
   apply_overrides(tex, int(scene.get(0, -1)), int(scene.get(1, -1)), batch_on, gui_env_mode, black_visible, disable_env, disable_env_spec)
}

fn normalize_pair(dict tex, int env_tex, int env_spec_tex, bool batch_on, bool proof_on, bool scene_needs_optical_spec) list {
   "Normalizes a selected environment pair for shader binding."
   mut out_env_tex = env_tex
   mut out_env_spec_tex = env_spec_tex
   if out_env_tex >= 0 && out_env_spec_tex < 0 { out_env_spec_tex = out_env_tex }
   if (batch_on || proof_on) && scene_needs_optical_spec && out_env_tex >= 0 { out_env_spec_tex = out_env_tex }
   if out_env_tex >= 0 && _tex(tex, "skybox") >= 0 && out_env_tex == _tex(tex, "skybox") &&
   _tex(tex, "skybox_spec") == _tex(tex, "skybox") && out_env_spec_tex != _tex(tex, "skybox"){
      out_env_spec_tex = _tex(tex, "skybox")
   }
   [out_env_tex, out_env_spec_tex]
}

fn generated_plan(
   dict tex,
   bool batch_on,
   bool proof_on,
   bool gui_probe_on,
   int gui_env_mode,
   bool studio_env,
   bool neutral_env,
   bool compare_reflect_env,
   bool compare_visible_env,
   bool optical_spec_env,
   bool feature_fallback_env
) list {
   "Returns generated-environment needs and requested variants."
   def force_generated = !batch_on && gui_env_mode > 0 && gui_env_mode < 4
   def needed = studio_env || neutral_env || compare_reflect_env ||
   compare_visible_env || optical_spec_env || feature_fallback_env
   def missing =
   (studio_env && _tex(tex, "compare_env") < 0) ||
   (neutral_env && _tex(tex, "neutral_env") < 0) ||
   (compare_visible_env && _tex(tex, "compare_visible_env") < 0) ||
   (compare_reflect_env && _tex(tex, "compare_reflect_spec") < 0) ||
   (feature_fallback_env && _tex(tex, "compare_env") < 0) ||
   (optical_spec_env && _tex(tex, "skybox") < 0)
   [
      (((batch_on || proof_on || gui_probe_on) && needed) || force_generated) && missing,
      studio_env || feature_fallback_env,
      compare_visible_env,
      neutral_env || feature_fallback_env,
      compare_reflect_env,
      optical_spec_env && _tex(tex, "skybox") < 0,
   ]
}

fn background_requested(
   bool skybox_enabled,
   bool compare_visible_env,
   bool batch_on,
   int gui_env_mode,
   bool gui_probe_on,
   bool gui_probe_has_scene,
   bool env_tex_ready,
   bool gui_enabled,
   bool black_visible,
   bool gui_draw_env_bg,
   bool proof_on,
   bool proof_skybox
) bool {
   "Returns whether an environment background should be drawn."
   mut draw_bg = skybox_enabled || compare_visible_env || (!batch_on && (gui_env_mode == 2 || gui_env_mode == 3))
   if gui_probe_on && !gui_probe_has_scene && !gui_draw_env_bg {
      draw_bg = false
   } elif gui_probe_on && gui_probe_has_scene && env_tex_ready && !black_visible {
      draw_bg = true
   } elif gui_enabled && !black_visible && gui_draw_env_bg {
      draw_bg = true
   }
   proof_on ? proof_skybox : draw_bg
}

fn proof_needs_generated(
   dict tex,
   int env_tex,
   int env_spec_tex,
   bool can_fallback,
   bool scene_has_lights,
   bool scene_env_sensitive_materials,
   bool scene_needs_reflect_spec
) bool {
   "Returns whether proof rendering needs generated fallback environments."
   if !can_fallback || env_tex >= 0 { return false }
   if scene_has_lights && !scene_env_sensitive_materials { return false }
   if _tex(tex, "compare_visible_env") < 0 { return true }
   if _tex(tex, "compare_env") < 0 { return true }
   if _tex(tex, "neutral_env") < 0 { return true }
   if env_spec_tex < 0 && _tex(tex, "compare_env_spec") < 0 &&
   _tex(tex, "compare_env") < 0 && _tex(tex, "neutral_env_spec") < 0{ return true }
   scene_needs_reflect_spec && _tex(tex, "compare_reflect_spec") < 0
}

fn proof_fallbacks(
   dict tex,
   int env_tex,
   int env_spec_tex,
   bool can_fallback,
   bool scene_has_lights,
   bool scene_env_sensitive_materials,
   bool scene_needs_reflect_spec,
   bool scene_needs_optical_spec,
   bool compare_visible_env
) list {
   "Applies environment fallback policy for proof rendering."
   mut out_env_tex = env_tex
   mut out_env_spec_tex = env_spec_tex
   if can_fallback && out_env_tex < 0 && (!scene_has_lights || scene_env_sensitive_materials) {
      if scene_needs_optical_spec && compare_visible_env && _tex(tex, "compare_visible_env") >= 0 {
         out_env_tex = _tex(tex, "compare_visible_env")
      } elif _tex(tex, "neutral_env") >= 0 {
         out_env_tex = _tex(tex, "neutral_env")
      } else {
         out_env_tex = first_ready([_tex(tex, "compare_env"), _tex(tex, "compare_visible_env")])
      }
      if out_env_spec_tex < 0 {
         if scene_needs_reflect_spec && _tex(tex, "compare_reflect_spec") >= 0 {
            out_env_spec_tex = _tex(tex, "compare_reflect_spec")
         } elif scene_needs_optical_spec {
            out_env_spec_tex = first_ready([_tex(tex, "skybox_spec"), _tex(tex, "skybox")])
         }
         if out_env_spec_tex < 0 {
            out_env_spec_tex = first_ready([
                  _tex(tex, "compare_env_spec"), _tex(tex, "compare_env"), _tex(tex, "neutral_env_spec"),
                  _tex(tex, "neutral_env"), _tex(tex, "compare_visible_env"),
            ])
         }
      }
   }
   [out_env_tex, out_env_spec_tex]
}

fn skybox_fallback(dict tex, int env_tex, int env_spec_tex, bool can_fallback) list {
   "Falls back to the skybox environment pair when allowed."
   mut out_env_tex = env_tex
   mut out_env_spec_tex = env_spec_tex
   if can_fallback && out_env_tex < 0 && _tex(tex, "skybox") >= 0 {
      out_env_tex = _tex(tex, "skybox")
      if out_env_spec_tex < 0 {
         out_env_spec_tex = (_tex(tex, "skybox_spec") >= 0) ? _tex(tex, "skybox_spec") : _tex(tex, "skybox")
      }
   }
   [out_env_tex, out_env_spec_tex]
}

mut _gltf_prefetch_mu = 0
mut _gltf_prefetch_data = dict(8)
mut _gltf_prefetch_order = []
mut _gltf_ogeom_cache = dict(8)
mut _gltf_ogeom_cache_order = []
mut _gltf_ogeom_cache_mu = 0
mut _gltf_tex_mips_cache = -1
def _GLTF_PREFETCH_LIMIT = 2
def _GLTF_OGEOM_CACHE_LIMIT = 8
def _SCENE_MAX_TEXTURE_SLOTS = MAX_TEXTURES
mut _MAT_UV_XFORM_PREFIXES = [
   "base_color", "normal", "metallic_roughness", "occlusion", "emissive",
   "clearcoat", "clearcoat_roughness", "clearcoat_normal",
   "sheen_color", "sheen_roughness", "anisotropy", "transmission",
   "iridescence", "iridescence_thickness", "specular", "specular_color",
   "thickness", "diffuse_transmission", "diffuse_transmission_color", "subsurface"
]

mut _MAT_TEX_SLOT_REQS = [["base_color", "base_color", "base_color"], ["normal", "normal", "normal"], ["metallic_roughness", "metallic_roughness", "metallic_roughness"], ["occlusion", "occlusion", "occlusion"], ["emissive", "emissive", "emissive"], ["specular", "specular", "occlusion"], ["specular_color", "specular_color", "emissive"], ["clearcoat", "clearcoat", "occlusion"], ["clearcoat_normal", "clearcoat_normal", "normal"], ["clearcoat_roughness", "clearcoat_roughness", "metallic_roughness"], ["sheen_color", "sheen_color", "emissive"], ["sheen_roughness", "sheen_roughness", "metallic_roughness"], ["transmission", "transmission", "metallic_roughness"], ["thickness", "thickness", "occlusion"], ["iridescence", "iridescence", "occlusion"], ["iridescence_thickness", "iridescence_thickness", "metallic_roughness"], ["anisotropy", "anisotropy", "metallic_roughness"], ["diffuse_transmission", "diffuse_transmission", "metallic_roughness"], ["diffuse_transmission_color", "diffuse_transmission_color", "emissive"]]
mut _MAT_SPEC_TEX_SLOTS = [["base_color", false], ["normal", true], ["metallic_roughness", true], ["occlusion", true], ["emissive", true], ["clearcoat", true], ["clearcoat_roughness", true], ["clearcoat_normal", true], ["sheen_color", true], ["sheen_roughness", true], ["anisotropy", true], ["transmission", true], ["iridescence", true], ["iridescence_thickness", true], ["specular", true], ["specular_color", true], ["thickness", true], ["diffuse_transmission", true], ["diffuse_transmission_color", true], ["subsurface", true]]
mut _MAT_SLOW_UV_STATE_FIELDS = [["base_uv_xf", "base_color"], ["normal_uv_xf", "normal"], ["mr_uv_xf", "metallic_roughness"], ["occ_uv_xf", "occlusion"], ["emit_uv_xf", "emissive"], ["spec_uv_xf", "specular"], ["spec_col_uv_xf", "specular_color"], ["thick_uv_xf", "thickness"], ["iri_uv_xf", "iridescence"], ["clearcoat_uv_xf", "clearcoat"], ["clearcoat_rough_uv_xf", "clearcoat_roughness"], ["clearcoat_normal_uv_xf", "clearcoat_normal"], ["sheen_color_uv_xf", "sheen_color"], ["sheen_rough_uv_xf", "sheen_roughness"], ["transmission_uv_xf", "transmission"], ["diffuse_transmission_uv_xf", "diffuse_transmission"], ["diffuse_transmission_color_uv_xf", "diffuse_transmission_color"], ["iri_thick_uv_xf", "iridescence_thickness"], ["anisotropy_uv_xf", "anisotropy"]]
mut _MAT_OCCLUSION_AUX_SLOTS = [["specular_id", "specular_texcoord", "spec_uv_xf", 0x01000000], ["sheen_color_id", "sheen_color_texcoord", "sheen_color_uv_xf", 0x40000000], ["clearcoat_id", "clearcoat_texcoord", "clearcoat_uv_xf", 0x20000000], ["thickness_id", "thickness_texcoord", "thick_uv_xf", 0x04000000], ["iridescence_id", "iridescence_texcoord", "iri_uv_xf", 0x08000000]]
mut _MAT_EMISSIVE_AUX_SLOTS = [["specular_color_id", "specular_color_texcoord", "spec_col_uv_xf", 0x02000000], ["diffuse_transmission_color_id", "diffuse_transmission_color_texcoord", "diffuse_transmission_color_uv_xf", 0x80000000]]
mut _MAT_RECORD_UV_FIELDS = [["base_uv_xf", "base_uv_xf0", "base_uv_xf1"], ["normal_uv_xf", "normal_uv_xf0", "normal_uv_xf1"], ["mr_uv_xf", "mr_uv_xf0", "mr_uv_xf1"], ["occ_uv_xf", "occlusion_uv_xf0", "occlusion_uv_xf1"], ["emit_uv_xf", "emissive_uv_xf0", "emissive_uv_xf1"]]
mut _MAT_FAST_SURFACE_ZERO_KEYS = [
   "clearcoat_factor", "clearcoat_roughness_factor", "sheen_roughness_factor",
   "anisotropy_strength", "anisotropy_rotation"
]

mut _MAT_FAST_VOLUME_ZERO_KEYS = [
   "transmission_factor", "iridescence_factor", "thickness_factor",
   "attenuation_distance", "dispersion", "diffuse_transmission_factor",
   "refraction_factor", "refraction_roughness", "subsurface_factor"
]

fn _stable_loaded_texture_id(any raw) int {
   def tid = int(raw)
   if tid < _SCENE_MAX_TEXTURE_SLOTS { return tid }
   def stable = render.texture_last_created_id()
   if stable >= 0 && stable < _SCENE_MAX_TEXTURE_SLOTS { return stable }
   return tid
}

mut _scene_merge_shared_i32_field_defaults_cache = 0
mut _scene_merge_i32_field_defaults_cache = 0
mut _scene_skin_solid_merge_i32_field_defaults_cache = 0

fn _scene_merge_shared_i32_field_defaults() list {
   if is_list(_scene_merge_shared_i32_field_defaults_cache) { return _scene_merge_shared_i32_field_defaults_cache }
   _scene_merge_shared_i32_field_defaults_cache = [["material_u32", 0x0000ff00], ["emissive_tex_id", -1], ["emissive_u32", 0], ["emissive_uv_set", 0], ["alpha_u32", 0], ["occlusion", -1], ["occlusion_uv_set", 0], ["normal_tex_id", -1], ["normal_uv_set", 0], ["bsdf0_u32", 0], ["bsdf1_u32", 0], ["bsdf2_u32", 0], ["bsdf3_u32", 0], ["bsdf4_u32", 0], ["bsdf5_u32", 0], ["ext2_tex_word", 0x80000000], ["base_uv_xf0", 0], ["base_uv_xf1", 0], ["normal_uv_xf0", 0], ["normal_uv_xf1", 0], ["mr_uv_xf0", 0], ["mr_uv_xf1", 0], ["occlusion_uv_xf0", 0], ["occlusion_uv_xf1", 0], ["emissive_uv_xf0", 0], ["emissive_uv_xf1", 0], ["vc_mode", 0]]
   _scene_merge_shared_i32_field_defaults_cache
}

fn _scene_merge_i32_field_defaults() list {
   if is_list(_scene_merge_i32_field_defaults_cache) { return _scene_merge_i32_field_defaults_cache }
   _scene_merge_i32_field_defaults_cache = [["primitive_mode", 4], ["mat_idx", -1], ["tex_id", -1], ["base_color_u32", 0xffffffff]].extend(_scene_merge_shared_i32_field_defaults())
   _scene_merge_i32_field_defaults_cache
}

mut _SCENE_MERGE_OPT_BOOL_FIELDS = ["is_lines", "is_points", "unlit", "no_cull", "double_sided", "flip_winding"]
def _SCENE_VC_VERTEX_TEX = 8
def _SCENE_VC_VERTEX_MATERIAL = 16

fn _scene_skin_solid_merge_i32_field_defaults() list {
   if is_list(_scene_skin_solid_merge_i32_field_defaults_cache) { return _scene_skin_solid_merge_i32_field_defaults_cache }
   _scene_skin_solid_merge_i32_field_defaults_cache = [["primitive_mode", 4], ["tex_id", -1]].extend(_scene_merge_shared_i32_field_defaults())
   _scene_skin_solid_merge_i32_field_defaults_cache
}

mut _SCENE_RENDER_REC_I32_FIELDS = [["base_color_u32", 12, 0xffffffff], ["material_u32", 13, 0x0000ff00], ["tex_id", 0, -1], ["emissive_tex_id", 14, -1], ["emissive_u32", 15, 0], ["emissive_uv_set", 16, 0], ["alpha_u32", 17, 0], ["occlusion", 18, -1], ["occlusion_tex_id", 18, -1], ["occlusion_uv_set", 19, 0], ["bsdf0_u32", 20, 0], ["bsdf1_u32", 21, 0], ["bsdf2_u32", 22, 0], ["bsdf3_u32", 23, 0], ["bsdf4_u32", 41, 0], ["bsdf5_u32", 42, 0], ["ext2_tex_word", 43, 0x80000000], ["normal_tex_id", 24, -1], ["normal_tex_word", 24, 0x80000000], ["normal_uv_set", 25, 0], ["base_uv_xf0", 26, 0], ["base_uv_xf1", 27, 0], ["normal_uv_xf0", 28, 0], ["normal_uv_xf1", 29, 0], ["mr_uv_xf0", 30, 0], ["mr_uv_xf1", 31, 0], ["occlusion_uv_xf0", 32, 0], ["occlusion_uv_xf1", 33, 0], ["emissive_uv_xf0", 34, 0], ["emissive_uv_xf1", 35, 0], ["node_idx", 11, -1]]

fn scene_asset_name_from_gltf_path(str path) str {
   "Returns the sample asset folder name for nested glTF/glTF-Binary paths."
   def dir_path = ospath.dirname(path)
   def dir_base = ospath.basename(dir_path)
   case dir_base {
      "glTF", "glTF-Binary", "glTF-Embedded", "glTF-Draco", "glTF-KTX-BasisU" -> {
         def parent = ospath.basename(ospath.dirname(dir_path))
         if parent.len > 0 { return parent }
      }
      _ -> {}
   }
   dir_base
}

fn _gltf_prefetch_mutex() any {
   if !_gltf_prefetch_mu { _gltf_prefetch_mu = mutex_new() }
   _gltf_prefetch_mu
}

fn _scene_ensure_runtime_caches() any {
   if !is_dict(_gltf_prefetch_data) { _gltf_prefetch_data = dict(8) }
   if !is_list(_gltf_prefetch_order) { _gltf_prefetch_order = [] }
   if !is_dict(_gltf_ogeom_cache) { _gltf_ogeom_cache = dict(8) }
   if !is_list(_gltf_ogeom_cache_order) { _gltf_ogeom_cache_order = [] }
   if !is_dict(_gltf_tex_cache) { _gltf_tex_cache = dict(64) }
   if !is_dict(_gltf_tex_resolve_cache) { _gltf_tex_resolve_cache = dict(256) }
}

fn _gltf_ogeom_cache_mutex() any {
   if !_gltf_ogeom_cache_mu { _gltf_ogeom_cache_mu = mutex_new() }
   _gltf_ogeom_cache_mu
}

fn _gltf_prefetch_store(str path, dict gltf_data) bool {
   if path.len == 0 || !is_dict(gltf_data) { return false }
   _scene_ensure_runtime_caches()
   def mu = _gltf_prefetch_mutex()
   if mu { mutex_lock(mu) }
   if !_gltf_prefetch_data.contains(path) { _gltf_prefetch_order = _gltf_prefetch_order.append(path) }
   _gltf_prefetch_data[path] = gltf_data
   while _gltf_prefetch_order.len > _GLTF_PREFETCH_LIMIT {
      def drop = to_str(_gltf_prefetch_order[0])
      _gltf_prefetch_order = slice(_gltf_prefetch_order, 1, _gltf_prefetch_order.len, 1)
      if drop.len > 0 { _gltf_prefetch_data = _gltf_prefetch_data.delete(drop) }
   }
   if mu { mutex_unlock(mu) }
   true
}

fn _gltf_prefetch_has(str path) bool {
   if path.len == 0 { return false }
   _scene_ensure_runtime_caches()
   def mu = _gltf_prefetch_mutex()
   if mu { mutex_lock(mu) }
   def found = _gltf_prefetch_data.contains(path)
   if mu { mutex_unlock(mu) }
   found
}

fn _gltf_tex_trace_enabled() bool {
   ui_profile.env_enabled_cached("NY_GLTF_TEX_TRACE")
}

fn _scene_stage_trace_enabled() bool {
   ui_profile.env_enabled_cached("NY_SCENE_STAGE_TRACE")
}

fn _scene_log_if(bool on, any line) bool {
   if !on { return false }
   ui_profile.print_text(to_str(line))
   true
}

fn _scene_stage(bool on, any detail) bool {
   _scene_log_if(on, "[scene:stage] " + to_str(detail))
}

fn _scene_stage_ms(bool on, any stage, any ms, any detail="") bool {
   if !on { return false }
   _scene_stage(on, to_str(stage) + " ms=" + to_str(ms) + to_str(detail))
}

fn _scene_prof(bool on, any detail) bool {
   _scene_log_if(on, "[gltf:prof] " + to_str(detail))
}

fn _scene_prof_ms(bool on, any stage, any ms, any detail="") bool {
   if !on { return false }
   _scene_prof(on, "stage=" + to_str(stage) + " ms=" + to_str(ms) + to_str(detail))
}

fn _scene_prof_elapsed(bool on, any stage, any t0, any detail="") bool {
   if !on { return false }
   _scene_prof_ms(on, stage, ui_profile.elapsed_ms(t0), detail)
}

fn _scene_prof_skip(bool on, any stage, any reason) bool {
   if !on { return false }
   _scene_prof_ms(on, stage, 0, " skipped=" + to_str(reason))
}

fn _scene_prof_enabled() bool {
   ui_profile.env_enabled_cached("NY_SCENE_PROFILE_TRACE") ||
   ui_profile.env_enabled_cached("NY_RENDER_PROFILE_TRACE") ||
   ui_profile.env_enabled_cached("NY_VK_PROFILE_TRACE") ||
   ui_profile.env_enabled_cached("NY_UI_PROFILE_TRACE")
}

fn _gltf_mat_summary_enabled() bool {
   ui_profile.env_enabled_cached("NY_GLTF_MAT_SUMMARY")
}

fn _scene_diag_enabled() bool {
   _gltf_debug_enabled() ||
   ui_profile.env_enabled_cached("NY_RENDER_PROFILE_TRACE") ||
   ui_profile.env_enabled_cached("NY_VK_PROFILE_TRACE") ||
   ui_profile.env_enabled_cached("NY_UI_PROFILE_TRACE")
}

fn _gltf_decode_threads() int {
   ui_profile.env_int_cached("NY_GLTF_DECODE_THREADS", 0, 0, 2147483647)
}

fn _scene_group_trace_enabled() bool {
   ui_profile.env_enabled_cached("NY_UI_GROUP_TRACE")
}

fn _scene_force_group_diag_enabled() bool {
   ui_profile.env_enabled_cached("NY_GLTF_FORCE_GROUP_DIAG")
}

fn _scene_prefix_list(any items, int limit) any {
   if !is_list(items) { return items }
   def n = min(items.len, max(0, limit))
   mut out = list(n)
   mut i = 0
   while i < n {
      out = out.append(items.get(i))
      i += 1
   }
   out
}

fn _scene_material_build_limit() int {
   if common.env_enabled("NY_GLTF_NODE_PERF_FULL") { return 0 }
   def raw = common.parse_nonneg_int(common.env_trim("NY_GLTF_NODE_PERF_PROOF_LIMIT"))
   if raw > 0 { return max(16, raw) }
   0
}

fn _scene_many_part_orbit_scan_limit() int {
   def raw = ui_profile.env_int_cached("NY_UI_MANY_PART_ORBIT_SCAN_LIMIT", 0, 0, 2147483647)
   raw > 0 ? raw : 1024
}

fn _scene_full_many_part_orbit_scan() bool {
   ui_profile.env_enabled_cached("NY_UI_FULL_MANY_PART_ORBIT_SCAN")
}

fn _scene_many_part_orbit_quick(int part_count) bool { part_count >= _scene_many_part_orbit_scan_limit() && !_scene_full_many_part_orbit_scan() }

fn _scene_tex_upload_trace_enabled() bool {
   ui_profile.env_truthy_cached("NY_SCENE_TEX_UPLOAD_TRACE")
}

fn _scene_material_feature_mask_from_infos(any material_infos) int {
   mut mat_mask = 0
   def mats_n = is_list(material_infos) ? material_infos.len : 0
   mut mi = 0
   while mi < mats_n {
      mat_mask = bor(mat_mask, gltf.gltf_material_feature_mask(material_infos.get(mi, 0)))
      mi += 1
   }
   mat_mask
}

fn _scene_gltf_material_count(any gltf_data) int {
   if !is_dict(gltf_data) { return 0 }
   def g = gltf_data.get("gltf", 0)
   if !is_dict(g) { return 0 }
   def mats = g.get("materials", 0)
   is_list(mats) ? mats.len : 0
}

fn _scene_set_mesh_limit(any gltf_data, int limit) bool {
   if !is_dict(gltf_data) || limit <= 0 { return false }
   gltf_data["__mesh_limit"] = limit
   gltf_data["__material_limit"] = limit
   def g = gltf_data.get("gltf", 0)
   if is_dict(g) {
      g["_ny_mesh_limit"] = limit
      gltf_data["gltf"] = g
   }
   true
}

fn _scene_merge_trace_enabled() bool {
   ui_profile.env_enabled_cached("NY_UI_MERGE_TRACE")
}

fn _scene_safe_count(any v) int {
   if is_list(v) { return v.len }
   0
}

fn _scene_anim_duration_valid(any v) bool {
   if !is_int(v)&& !is_float(v) { return false }
   if is_nan(v)|| is_inf(v) { return false }
   def out = 0.0 + v
   out > 0.0001 && out < 3600.0
}

fn _scene_anim_duration_from_doc(any doc) f64 {
   if !is_dict(doc) { return 0.0 }
   def anims = doc.get("animations", 0)
   def accs = doc.get("accessors", 0)
   if !is_list(anims)|| !is_list(accs) { return 0.0 }
   mut duration = 0.0
   mut ai = 0
   def anims_n = anims.len
   def accs_n = accs.len
   while ai < anims_n {
      def anim = anims.get(ai, 0)
      def samplers = anim.get("samplers", [])
      mut si = 0
      def samplers_n = samplers.len
      while si < samplers_n {
         def samp = samplers.get(si, 0)
         def input_idx = int(samp.get("input", -1))
         if input_idx >= 0 && input_idx < accs_n {
            def input_acc = accs.get(input_idx, 0)
            if is_dict(input_acc) {
               def acc_max = input_acc.get("max", 0)
               if is_list(acc_max) && acc_max.len > 0 {
                  def last_t = float(acc_max.get(0, 0.0))
                  if _scene_anim_duration_valid(last_t)&& last_t > duration { duration = last_t }
               }
            }
         }
         si += 1
      }
      ai += 1
   }
   _scene_anim_duration_valid(duration) ? duration : 0.0
}

fn _scene_anim_duration_from_source(any gltf_data) f64 {
   def source_path = to_str(gltf_data.get("source_path", ""))
   if source_path.len <= 0 || !osfs.is_file(source_path) { return 0.0 }
   def raw_res = file_read(source_path)
   if is_err(raw_res) { return 0.0 }
   _scene_anim_duration_from_doc(json_decode(unwrap(raw_res)))
}

fn _scene_anim_duration_from_accessors(any gltf_data) f64 {
   def g0 = gltf_data.get("gltf", 0)
   if !is_dict(g0) { return 0.0 }
   def anims0 = g0.get("animations", 0)
   if !is_list(anims0) || anims0.len <= 0 { return 0.0 }
   def anim0 = anims0.get(0, 0)
   if !is_dict(anim0) { return 0.0 }
   def samps0 = anim0.get("samplers", [])
   def accs = g0.get("accessors", 0)
   if !is_list(samps0) || !is_list(accs) { return 0.0 }
   mut adur = 0.0
   mut si = 0
   def samps_n = samps0.len
   while si < samps_n {
      def samp = samps0.get(si, 0)
      def input_idx = is_dict(samp) ? int(samp.get("input", -1)) : -1
      def input_acc = input_idx >= 0 && input_idx < accs.len ? accs.get(input_idx, 0) : 0
      if is_dict(input_acc) {
         def acc_max = input_acc.get("max", 0)
         if is_list(acc_max) && acc_max.len > 0 {
            def last_t = float(acc_max.get(0, 0.0))
            if _scene_anim_duration_valid(last_t)&& last_t > adur { adur = last_t }
         }
      }
      si += 1
   }
   adur
}

fn _scene_first_anim_duration(any gltf_data, int anim_cnt) f64 {
   "Returns the playback duration for the scene animation clock."
   if anim_cnt <= 0 { return 0.0 }
   mut adur = 0.0
   mut ai = 0
   while ai < anim_cnt {
      def anim_info = gltf.gltf_animation_info(gltf_data, ai)
      def dur = is_dict(anim_info) ? float(anim_info.get("duration", 0.0)) : 0.0
      if _scene_anim_duration_valid(dur) && dur > adur { adur = dur }
      ai += 1
   }
   if adur <= 0.0001 {
      def raw_dur = _scene_anim_duration_from_source(gltf_data)
      if raw_dur > adur { adur = raw_dur }
   }
   if adur <= 0.0001 {
      def acc_dur = _scene_anim_duration_from_accessors(gltf_data)
      if acc_dur > adur { adur = acc_dur }
   }
   if !_scene_anim_duration_valid(adur) { return 0.0 }
   adur
}

fn _scene_mark_fit_dirty_full(dict scene) dict {
   scene["parts_model_baked"] = false
   scene["gpu_model_baked"] = false
   scene["fit_applied"] = false
   scene
}

fn _scene_mark_fit_dirty_parts(dict scene) dict {
   scene["parts_model_baked"] = false
   scene["fit_applied"] = false
   scene
}

fn _scene_gpu_state_clear_model_flag(any gpu_state) any {
   if is_list(gpu_state) && gpu_state.len >= 9 {
      gpu_state[5] = 0
   } elif is_list(gpu_state) && gpu_state.len >= 7 {
      gpu_state[4] = 0
   }
   gpu_state
}

fn _scene_gpu_state_set_lights(any gpu_state, any lights_slab, int lights_count) any {
   if is_list(gpu_state) && gpu_state.len >= 9 {
      gpu_state[6] = lights_slab
      gpu_state[7] = lights_count
   } elif is_list(gpu_state) && gpu_state.len >= 7 {
      gpu_state[5] = lights_slab
      gpu_state[6] = lights_count
   }
   gpu_state
}

fn _scene_anim_sample_time(any now_sec, any anim_dur) f64 {
   mut t = float(now_sec)
   if is_nan(t)|| is_inf(t) { t = 0.0 }
   if abs(t)> 1000000.0 { t = 0.0 }
   if anim_dur > 0.0 {
      while t >= anim_dur { t -= anim_dur }
      while t < 0.0 { t += anim_dur }
   }
   t
}

fn _scene_anim_overrides(any gltf_data, int anim_count, int anim_idx, f64 t) dict {
   if anim_count <= 0 { return dict(0) }
   if anim_idx < 0 { return gltf.gltf_sample_animation_merged(gltf_data, t) }
   gltf.gltf_sample_animation(gltf_data, anim_idx, t)
}

fn _scene_apply_anim_morphs(dict scene, any gltf_data, dict overrides, any mat_records, any parts, int morph_count_total) list {
   if morph_count_total <= 0 { return [scene, gltf_data, parts] }
   def morph_apply = gltf.gltf_apply_morph_weights(gltf_data, overrides)
   def morph_gltf = morph_apply.get(0, gltf_data)
   def morph_changed = morph_apply.get(1, false) ? true : false
   if is_dict(morph_gltf) {
      gltf_data = morph_gltf
      scene["gltf_data"] = gltf_data
   }
   if morph_changed {
      def rebuilt = gltf.gltf_to_mesh_group_indexed(gltf_data, 0, mat_records)
      if is_dict(rebuilt) && is_list(rebuilt.get("parts", 0)) {
         def raw_parts = rebuilt.get("parts", parts)
         if is_list(parts) && parts.len == raw_parts.len && parts.len > 0 {
            parts = render_utils.gltf_sync_drawable_parts_from_raw(parts, raw_parts, false, false)
         } else {
            parts = _scene_rebuild_drawable_parts_from_raw(raw_parts)
         }
         scene["parts"] = parts
         scene["parts_count"] = is_list(parts) ? parts.len : 0
         scene["min"] = rebuilt.get("min", scene.get("min", 0))
         scene["max"] = rebuilt.get("max", scene.get("max", 0))
         scene["gpu_model_baked"] = false
         def gpu_state = scene.get("gpu_draw_state", 0)
         scene["gpu_draw_state"] = _scene_gpu_state_clear_model_flag(gpu_state)
      }
   }
   [scene, gltf_data, parts]
}

fn _scene_apply_anim_part(dict part, any gltf_data, dict anim_mats, any vis_map, bool use_vis_map, any ptr_overrides, any skin_mats_cache) dict {
   def node_idx = int(part.get("node_idx", -1))
   def skin_idx = int(part.get("skin_idx", -1))
   if node_idx >= 0 {
      def next_model = anim_mats.get(node_idx, anim_mats.get(to_str(node_idx), 0))
      if is_list(next_model) { part["model"] = next_model }
      if use_vis_map { part["visible"] = vis_map.get(node_idx, true) ? true : false }
   }
   if is_list(ptr_overrides)&& ptr_overrides.len > 0 { part = render_utils.gltf_anim_apply_material_pointer_overrides(part, ptr_overrides) }
   if skin_idx >= 0 {
      part = gltf.gltf_apply_skinning(part, gltf_data, anim_mats, skin_mats_cache)
      if common.env_truthy("NY_GLTF_SKIN_MODEL_IDENTITY") {
         part["model"] = [
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0,
            "mat4", 400
         ]
      }
   }
   part
}

fn _scene_apply_anim_parts(list parts, any gltf_data, dict anim_mats, any vis_map, bool use_vis_map, any ptr_overrides, int skin_count, bool fast_numeric_anim=false) list {
   def skin_mats_cache = (skin_count > 0 && !fast_numeric_anim) ? dict(max(4, skin_count * 2)) : 0
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      mut part = parts.get(i, 0)
      if is_dict(part) {
         part = _scene_apply_anim_part(part, gltf_data, anim_mats, vis_map, use_vis_map, ptr_overrides, skin_mats_cache)
         parts[i] = part
      }
      i += 1
   }
   gltf.gltf_free_skin_mats_cache(skin_mats_cache)
   parts
}

fn _scene_refresh_anim_lights(dict scene, any gltf_data, dict overrides) dict {
   def fit_scale = float(scene.get("fit_scale", 1.0))
   def fit_tx = float(scene.get("fit_tx", 0.0))
   def fit_ty = float(scene.get("fit_ty", 0.0))
   def fit_tz = float(scene.get("fit_tz", 0.0))
   mut scene_lights = _scene_limit_lights(gltf.gltf_scene_punctual_lights(gltf_data, overrides), _SCENE_LIGHT_MAX)
   if !is_list(scene_lights) { scene_lights = [] }
   def old_scene_lights_slab = scene.get("scene_lights_slab", 0)
   def scene_lights_slab = _pack_scene_lights_slab(scene_lights, fit_scale, fit_tx, fit_ty, fit_tz, true)
   def material_feature_mask = int(scene.get("material_feature_mask", 0))
   def scene_env_sensitive_materials = band(material_feature_mask, _SCENE_ENV_SENSITIVE_MASK) != 0
   if old_scene_lights_slab && old_scene_lights_slab != scene_lights_slab { free(old_scene_lights_slab) }
   scene["scene_lights"] = scene_lights
   scene["scene_lights_slab"] = scene_lights_slab
   scene["scene_lights_count"] = scene_lights.len
   scene["material_feature_mask"] = material_feature_mask
   scene["scene_env_sensitive_materials"] = scene_env_sensitive_materials
   mut gpu_state_anim = scene.get("gpu_draw_state", 0)
   gpu_state_anim = _scene_gpu_state_clear_model_flag(gpu_state_anim)
   gpu_state_anim = _scene_gpu_state_set_lights(gpu_state_anim, scene_lights_slab, scene_lights.len)
   scene["gpu_draw_state"] = gpu_state_anim
   scene
}

fn _scene_parts_keep_gltf_node_identity(any parts, any raw_parts) bool {
   "Checks whether draw-ready rigid glTF parts still have the node ids needed for nested animation."
   if !is_list(parts) || !is_list(raw_parts) { return false }
   if parts.len <= 0 || parts.len != raw_parts.len { return false }
   mut seen = dict(max(4, parts.len * 2))
   mut ri = 0
   while ri < raw_parts.len {
      def rp = raw_parts.get(ri, 0)
      if is_dict(rp) {
         def rn = int(rp.get("node_idx", -1))
         if rn >= 0 { seen[rn] = int(seen.get(rn, 0)) + 1 }
      }
      ri += 1
   }
   if seen.len <= 0 { return false }
   mut matched = 0
   mut pi = 0
   while pi < parts.len {
      def p = parts.get(pi, 0)
      if !is_dict(p) { return false }
      def pn = int(p.get("node_idx", -1))
      if pn < 0 { return false }
      def count = int(seen.get(pn, 0))
      if count <= 0 { return false }
      seen[pn] = count - 1
      matched += 1
      pi += 1
   }
   matched == parts.len
}

fn _scene_apply_gltf_animation(any scene, int anim_idx=0, f64 now_sec=0.0) any {
   "Applies glTF animation overrides to part visibility and model matrices in-place."
   if !is_dict(scene) { return scene }
   mut gltf_data = scene.get("gltf_data", 0)
   if !is_dict(gltf_data) { return scene }
   def anim_count = int(scene.get("anim_count", 0))
   def skin_count = int(scene.get("skin_count", 0))
   def morph_count_total = int(scene.get("morph_target_count", 0))
   if anim_count <= 0 && skin_count <= 0 && morph_count_total <= 0 { return scene }
   mut anim_dur = float(scene.get("anim_duration", 0.0))
   if !_scene_anim_duration_valid(anim_dur) { anim_dur = 0.0 }
   def t = _scene_anim_sample_time(now_sec, anim_dur)
   def was_playing = bool(scene.get("anim_playing", false))
   def explicit_time = bool(scene.get("anim_time_override", false)) || now_sec > 0.0 || was_playing
   scene["anim_idx"] = anim_idx
   scene["anim_time"] = t
   scene["anim_time_override"] = explicit_time
   scene["anim_playing"] = was_playing || now_sec > 0.0
   if explicit_time {
      scene["static_pose_gpu_ready"] = false
      scene["parts_model_baked"] = false
      scene["gpu_model_baked"] = false
      def gpu_state_anim_pose = scene.get("gpu_draw_state", 0)
      scene["gpu_draw_state"] = _scene_gpu_state_clear_model_flag(gpu_state_anim_pose)
   }
   def overrides = _scene_anim_overrides(gltf_data, anim_count, anim_idx, t)
   if !is_dict(overrides) { return scene }
   scene = _scene_mark_fit_dirty_full(scene)
   mut anim_mats = gltf.gltf_rebuild_animated_mats(gltf_data, overrides)
   if common.env_truthy("NY_GLTF_SKIN_BIND_POSE") { anim_mats = gltf.gltf_rebuild_animated_mats(gltf_data, dict(0)) }
   def fast_numeric_anim = overrides.get("__fast_numeric", false) ? true : false
   def use_vis_map = fast_numeric_anim ? false : gltf.gltf_has_node_visibility(gltf_data, overrides)
   def vis_map = use_vis_map ? gltf.gltf_resolve_node_visibility(gltf_data, overrides) : 0
   def ptr_overrides = overrides.get("__pointers", [])
   def mat_records = scene.get("mat_records", [])
   mut parts = scene.get("parts", 0)
   def raw_anim_parts = scene.get("anim_raw_parts", 0)
   def rigid_node_anim = anim_count > 0 && skin_count <= 0 && morph_count_total <= 0 && is_list(raw_anim_parts) && raw_anim_parts.len > 0
   if rigid_node_anim {
      ;; Rigid glTF animation is node-matrix animation.  Keep drawables aligned
      ;; with the original unmerged glTF primitive list so each part keeps the
      ;; node_idx needed for parent->child world transforms.  Do not rebuild the
      ;; whole glTF mesh every frame; that is slow and can double-apply sampled
      ;; matrices before the normal part animation pass below.
      if !_scene_parts_keep_gltf_node_identity(parts, raw_anim_parts) {
         parts = _scene_rebuild_drawable_parts_from_raw(raw_anim_parts)
      }
      scene["parts"] = parts
      scene["parts_count"] = is_list(parts) ? parts.len : 0
      scene["static_pose_gpu_ready"] = false
      scene["parts_model_baked"] = false
      scene["gpu_model_baked"] = false
      scene["gpu_parts"] = []
      scene["gpu_parts_slab"] = 0
      scene["gpu_parts_count"] = 0
      scene["gpu_draw_state"] = _scene_gpu_state_clear_model_flag(scene.get("gpu_draw_state", 0))
   }
   def morph_state = _scene_apply_anim_morphs(scene, gltf_data, overrides, mat_records, parts, morph_count_total)
   scene, gltf_data, parts = morph_state.get(0, scene), morph_state.get(1, gltf_data), morph_state.get(2, parts)
   if !is_list(parts) { return scene }
   parts = _scene_apply_anim_parts(parts, gltf_data, anim_mats, vis_map, use_vis_map, ptr_overrides, skin_count, fast_numeric_anim)
   scene["parts"] = parts
   scene["parts_count"] = parts.len
   scene = _scene_mark_fit_dirty_parts(scene)
   _scene_refresh_anim_lights(scene, gltf_data, overrides)
}

fn apply_gltf_animation(any scene, int anim_idx=0, f64 now_sec=0.0) any { return _scene_apply_gltf_animation(scene, anim_idx, now_sec) }

fn first_gltf_in_dir(str dir, str prefer="") str { asset_catalog.first_gltf_in_dir(dir, prefer) }

fn prefetch_gltf_asset_path(str gltf_path) bool {
   "Preloads and parses an already-resolved glTF/GLB file path."
   if gltf_path.len == 0 { return false }
   def already_prefetched = _gltf_prefetch_has(gltf_path)
   if already_prefetched { return already_prefetched }
   mut prefetched = gltf.load_gltf_file(gltf_path)
   if !is_dict(prefetched) { return false }
   def prepared_mesh = gltf.gltf_to_mesh_group_indexed(prefetched, 0, 0)
   if is_dict(prepared_mesh) { prefetched["__prepared_raw_mesh"] = _clone_cached_mesh(prepared_mesh) }
   _gltf_prefetch_store(gltf_path, prefetched)
}

fn format_name_list(any items) str {
   "Joins a list of strings with comma separators."
   asset_catalog.format_name_list(items)
}

fn _scene_auto_fit_enabled() bool {
   def v = common.env_lower("NY_GLTF_AUTO_FIT")
   if v.len > 0 { return !common.env_falsey("NY_GLTF_AUTO_FIT") }
   !common.env_enabled("NY_GLTF_DISABLE_AUTO_FIT")
}

fn _gltf_debug_enabled() bool { common.env_enabled("NY_GLTF_DEBUG") || common.env_enabled("NY_GLTF_MODEL_DEBUG") || common.env_enabled("NY_DEBUG_DEEP") }

fn _identity_model_mat4() list { [4, 4, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0] }

fn _as_render_model_mat4(any model_m) list {
   "Normalizes model matrices to the renderer [4,4,...16 floats] layout."
   if !is_list(model_m) { return _identity_model_mat4() }
   def n = model_m.len
   if n == 18 && int(model_m.get(0, 0)) == 4 && int(model_m.get(1, 0)) == 4 { return model_m }
   if n == 18 && is_str(model_m.get(16, 0)) {
      return [4, 4,
         float(model_m.get(0, 0.0)), float(model_m.get(1, 0.0)),
         float(model_m.get(2, 0.0)), float(model_m.get(3, 0.0)),
         float(model_m.get(4, 0.0)), float(model_m.get(5, 0.0)),
         float(model_m.get(6, 0.0)), float(model_m.get(7, 0.0)),
         float(model_m.get(8, 0.0)), float(model_m.get(9, 0.0)),
         float(model_m.get(10, 0.0)), float(model_m.get(11, 0.0)),
         float(model_m.get(12, 0.0)), float(model_m.get(13, 0.0)),
         float(model_m.get(14, 0.0)), float(model_m.get(15, 0.0))
      ]
   }
   if n == 16 {
      return [4, 4,
         float(model_m.get(0, 0.0)), float(model_m.get(1, 0.0)),
         float(model_m.get(2, 0.0)), float(model_m.get(3, 0.0)),
         float(model_m.get(4, 0.0)), float(model_m.get(5, 0.0)),
         float(model_m.get(6, 0.0)), float(model_m.get(7, 0.0)),
         float(model_m.get(8, 0.0)), float(model_m.get(9, 0.0)),
         float(model_m.get(10, 0.0)), float(model_m.get(11, 0.0)),
         float(model_m.get(12, 0.0)), float(model_m.get(13, 0.0)),
         float(model_m.get(14, 0.0)), float(model_m.get(15, 0.0))
      ]
   }
   _identity_model_mat4()
}

fn _scene_fit_model_mat4(any scale, any tx, any ty, any tz) list {
   def M_PT, M_PS = mat4_identity(), mat4_identity()
   def M_SP = mat4_identity()
   mat4_translate_into(tx, ty, tz, M_PT)
   mat4_scale_into(scale, scale, scale, M_PS)
   mat4_mul_into(M_PT, M_PS, M_SP)
   M_SP
}

fn _scene_float_bad(any v) bool {
   if !is_int(v)&& !is_float(v) { return true }
   if is_nan(v)|| is_inf(v) { return true }
   abs(0.0 + v) > 1000000.0
}

fn _scene_float3_bad(any x, any y, any z) bool {
   _scene_float_bad(x) || _scene_float_bad(y) || _scene_float_bad(z)
}

fn _scene_float6_bad(any x1, any y1, any z1, any x2, any y2, any z2) bool {
   _scene_float3_bad(x1, y1, z1) || _scene_float3_bad(x2, y2, z2)
}

fn _scene_list_has_bad_float(any xs, int limit=-1) bool {
   if !is_list(xs) { return false }
   def n = (limit >= 0 && limit < xs.len) ? limit : xs.len
   mut i = 0
   while i < n {
      if _scene_float_bad(xs[i]) { return true }
      i += 1
   }
   false
}

fn _scene_bounds_ok(any pmin, any pmax) bool {
   if !is_list(pmin)|| !is_list(pmax) || pmin.len < 3 || pmax.len < 3 { return false }
   def mnx, mny = _num_or(pmin.get(0, 0.0), 0.0), _num_or(pmin.get(1, 0.0), 0.0)
   def mnz = _num_or(pmin.get(2, 0.0), 0.0)
   def mxx = _num_or(pmax.get(0, 0.0), 0.0)
   def mxy = _num_or(pmax.get(1, 0.0), 0.0)
   def mxz = _num_or(pmax.get(2, 0.0), 0.0)
   if _scene_float6_bad(mnx, mny, mnz, mxx, mxy, mxz) { return false }
   if mnx > mxx || mny > mxy || mnz > mxz { return false }
   def dx, dy = abs(mxx - mnx), abs(mxy - mny)
   def dz = abs(mxz - mnz)
   if max(dx, max(dy, dz)) < 0.000000001 { return false }
   true
}

fn _scene_transform_aabb(any pmin, any pmax, any model_m) list {
   if !_scene_bounds_ok(pmin, pmax) { return [] }
   if !is_list(model_m) { return [pmin, pmax] }
   def n = model_m.len
   mut base = -1
   if n == 18
   && (is_int(model_m.get(0, 0)) || is_float(model_m.get(0, 0)))
   && (is_int(model_m.get(1, 0)) || is_float(model_m.get(1, 0)))
   && int(model_m.get(0, 0)) == 4
   && int(model_m.get(1, 0)) == 4{
      base = 2
   }
   elif n == 18 && is_str(model_m.get(16, 0)) {
      base = 0
   }
   elif n >= 16 {
      base = 0
   }
   if base < 0 { return [pmin, pmax] }
   def x1, y1 = _num_or(pmin.get(0, 0.0), 0.0), _num_or(pmin.get(1, 0.0), 0.0)
   def z1 = _num_or(pmin.get(2, 0.0), 0.0)
   def x2 = _num_or(pmax.get(0, 0.0), 0.0)
   def y2 = _num_or(pmax.get(1, 0.0), 0.0)
   def z2 = _num_or(pmax.get(2, 0.0), 0.0)
   def m00 = _num_or(model_m.get(base + 0, 1.0), 1.0)
   def m01 = _num_or(model_m.get(base + 4, 0.0), 0.0)
   def m02 = _num_or(model_m.get(base + 8, 0.0), 0.0)
   def m03 = _num_or(model_m.get(base + 12, 0.0), 0.0)
   def m10 = _num_or(model_m.get(base + 1, 0.0), 0.0)
   def m11 = _num_or(model_m.get(base + 5, 1.0), 1.0)
   def m12 = _num_or(model_m.get(base + 9, 0.0), 0.0)
   def m13 = _num_or(model_m.get(base + 13, 0.0), 0.0)
   def m20 = _num_or(model_m.get(base + 2, 0.0), 0.0)
   def m21 = _num_or(model_m.get(base + 6, 0.0), 0.0)
   def m22 = _num_or(model_m.get(base + 10, 1.0), 1.0)
   def m23 = _num_or(model_m.get(base + 14, 0.0), 0.0)
   if _scene_list_has_bad_float([m00, m01, m02, m03, m10, m11, m12, m13, m20, m21, m22, m23]) { return [] }
   mut wmin_x, wmin_y = 1e9, 1e9
   mut wmin_z = 1e9
   mut wmax_x = -1e9
   mut wmax_y = -1e9
   mut wmax_z = -1e9
   mut used = 0
   mut ci = 0
   while ci < 8 {
      def px, py = (band(ci, 1) != 0) ? x2 : x1, (band(ci, 2) != 0) ? y2 : y1
      def pz = (band(ci, 4) != 0) ? z2 : z1
      def wx = 0.0 + (m00 * px + m01 * py + m02 * pz + m03)
      def wy = 0.0 + (m10 * px + m11 * py + m12 * pz + m13)
      def wz = 0.0 + (m20 * px + m21 * py + m22 * pz + m23)
      if !_scene_float3_bad(wx, wy, wz) {
         if wx < wmin_x { wmin_x = 0.0 + wx }
         if wx > wmax_x { wmax_x = 0.0 + wx }
         if wy < wmin_y { wmin_y = 0.0 + wy }
         if wy > wmax_y { wmax_y = 0.0 + wy }
         if wz < wmin_z { wmin_z = 0.0 + wz }
         if wz > wmax_z { wmax_z = 0.0 + wz }
         used += 1
      }
      ci += 1
   }
   if used <= 0 { return [] }
   return [[wmin_x, wmin_y, wmin_z], [wmax_x, wmax_y, wmax_z]]
}

fn _scene_bounds_from_vertex_ptr(any vptr, int vcnt) list {
   if !vptr || vcnt <= 0 { return [] }
   mut min_x, min_y = 1e9, 1e9
   mut min_z = 1e9
   mut max_x = -1e9
   mut max_y = -1e9
   mut max_z = -1e9
   mut used = 0
   mut vi = 0
   while vi < vcnt {
      def off = vi * VERTEX_STRIDE
      def px = 0.0 + load32_f32(vptr, off + _VKR_OFF_X)
      def py = 0.0 + load32_f32(vptr, off + _VKR_OFF_Y)
      def pz = 0.0 + load32_f32(vptr, off + _VKR_OFF_Z)
      if !_scene_float3_bad(px, py, pz) {
         if px < min_x { min_x = 0.0 + px }
         if px > max_x { max_x = 0.0 + px }
         if py < min_y { min_y = 0.0 + py }
         if py > max_y { max_y = 0.0 + py }
         if pz < min_z { min_z = 0.0 + pz }
         if pz > max_z { max_z = 0.0 + pz }
         used += 1
      }
      vi += 1
   }
   if used <= 0 || min_x > max_x || min_y > max_y || min_z > max_z { return [] }
   return [[min_x, min_y, min_z], [max_x, max_y, max_z]]
}

fn _scene_bounds_accum_new() list {
   mut out = []
   out = out.append(1000000000.0)
   out = out.append(1000000000.0)
   out = out.append(1000000000.0)
   out = out.append(-1000000000.0)
   out = out.append(-1000000000.0)
   out = out.append(-1000000000.0)
   out
}

fn _scene_bounds_accum(any state0, any pmin, any pmax) list {
   mut state = state0
   if !is_list(state) || state.len < 6 { state = _scene_bounds_accum_new() }
   if !is_list(pmin) || !is_list(pmax) || pmin.len < 3 || pmax.len < 3 { return state }
   def mnx, mny = float(pmin.get(0, 0)), float(pmin.get(1, 0))
   def mnz = float(pmin.get(2, 0))
   def mxx, mxy = float(pmax.get(0, 0)), float(pmax.get(1, 0))
   def mxz = float(pmax.get(2, 0))
   mut out = []
   out = out.append(mnx < float(state.get(0, 1000000000.0)) ? mnx : float(state.get(0, 1000000000.0)))
   out = out.append(mny < float(state.get(1, 1000000000.0)) ? mny : float(state.get(1, 1000000000.0)))
   out = out.append(mnz < float(state.get(2, 1000000000.0)) ? mnz : float(state.get(2, 1000000000.0)))
   out = out.append(mxx > float(state.get(3, -1000000000.0)) ? mxx : float(state.get(3, -1000000000.0)))
   out = out.append(mxy > float(state.get(4, -1000000000.0)) ? mxy : float(state.get(4, -1000000000.0)))
   out = out.append(mxz > float(state.get(5, -1000000000.0)) ? mxz : float(state.get(5, -1000000000.0)))
   out
}

fn _scene_bounds_accum_part(list state, any part, any bounds=0) list {
   def pmin = is_list(bounds) ? bounds.get(0, [0.0, 0.0, 0.0]) : part.get("min", [0.0, 0.0, 0.0])
   def pmax = is_list(bounds) ? bounds.get(1, [0.0, 0.0, 0.0]) : part.get("max", [0.0, 0.0, 0.0])
   _scene_bounds_accum(state, pmin, pmax)
}

fn _scene_bounds_accum_result(list state) list {
   [[state.get(0, 0.0), state.get(1, 0.0), state.get(2, 0.0)],
   [state.get(3, 0.0), state.get(4, 0.0), state.get(5, 0.0)]]
}

fn _scene_part_front_bias_xz(any part) list {
   if !is_dict(part) { return [0.0, 0.0, 0.0] }
   def vptr = part.get("vptr", 0)
   def vcnt = int(part.get("vcnt", 0))
   if !vptr || vcnt <= 0 { return [0.0, 0.0, 0.0] }
   def model_m = _as_render_model_mat4(part.get("model", 0))
   def m00, m10 = float(model_m.get(2, 1.0)), float(model_m.get(3, 0.0))
   def m20 = float(model_m.get(4, 0.0))
   def m01 = float(model_m.get(6, 0.0))
   def m11 = float(model_m.get(7, 1.0))
   def m21 = float(model_m.get(8, 0.0))
   def m02 = float(model_m.get(10, 0.0))
   def m12 = float(model_m.get(11, 0.0))
   def m22 = float(model_m.get(12, 1.0))
   mut sum_x, sum_z = 0.0, 0.0
   mut used = 0
   def step = max(1, int(vcnt / 128))
   mut vi = 0
   while vi < vcnt {
      def off = vi * VERTEX_STRIDE
      def nx = load32_f32(vptr, off + _VKR_OFF_NX)
      def ny = load32_f32(vptr, off + _VKR_OFF_NY)
      def nz = load32_f32(vptr, off + _VKR_OFF_NZ)
      def nlen2 = nx * nx + ny * ny + nz * nz
      if nlen2 > 0.000001 {
         def wx, wy = m00 * nx + m01 * ny + m02 * nz, m10 * nx + m11 * ny + m12 * nz
         def wz = m20 * nx + m21 * ny + m22 * nz
         def wlen2 = wx * wx + wy * wy + wz * wz
         if wlen2 > 0.000001 {
            def inv = 1.0 / sqrt(wlen2)
            sum_x += -(wx * inv)
            sum_z += -(wz * inv)
            used += 1
         }
      }
      vi += step
   }
   [sum_x, sum_z, used]
}

fn _scene_part_front_weight(any part) f64 {
   if !is_dict(part) { return 1.0 }
   mut vcnt_floor = 1.0
   def vcnt = int(part.get("vcnt", 0))
   if vcnt > 0 { vcnt_floor = max(1.0, float(vcnt) / 256.0) }
   def pmin = part.get("min", 0)
   def pmax = part.get("max", 0)
   if is_list(pmin) && is_list(pmax) && pmin.len >= 3 && pmax.len >= 3 {
      def dx = abs(float(pmax.get(0, 0.0)) - float(pmin.get(0, 0.0)))
      def dy = abs(float(pmax.get(1, 0.0)) - float(pmin.get(1, 0.0)))
      def dz = abs(float(pmax.get(2, 0.0)) - float(pmin.get(2, 0.0)))
      def max_dim = max(dx, max(dy, dz))
      def min_dim = min(dx, min(dy, dz))
      def area_like = max(dx * dy, max(dx * dz, dy * dz))
      if area_like > 0.0001 {
         if max_dim > 0.0001 && min_dim < max_dim * 0.08 {
            return max(area_like * 8.0, vcnt_floor * 4.0)
         }
         return max(area_like, vcnt_floor)
      }
   }
   vcnt_floor
}

fn _scene_bounds_is_backdrop(any pmin, any pmax, f64 scene_area_like) bool {
   if scene_area_like <= 0.0001 { return false }
   if !is_list(pmin)|| !is_list(pmax) || pmin.len < 3 || pmax.len < 3 { return false }
   def dx = abs(float(pmax.get(0, 0)) - float(pmin.get(0, 0)))
   def dy = abs(float(pmax.get(1, 0)) - float(pmin.get(1, 0)))
   def dz = abs(float(pmax.get(2, 0)) - float(pmin.get(2, 0)))
   def max_dim = max(dx, max(dy, dz))
   def min_dim = min(dx, min(dy, dz))
   if max_dim <= 0.0001 || min_dim >= max_dim * 0.08 { return false }
   def area_like = max(dx * dy, max(dx * dz, dy * dz))
   area_like >= scene_area_like * 0.18
}

fn _scene_part_is_backdrop(any part, f64 scene_area_like) bool {
   if !is_dict(part)|| scene_area_like <= 0.0001 { return false }
   _scene_bounds_is_backdrop(part.get("min", 0), part.get("max", 0), scene_area_like)
}

fn _scene_subject_bounds_from_parts(any mesh, f64 scene_area_like) list {
   if !is_dict(mesh) || scene_area_like <= 0.0001 { return [0, 0, 0, 0] }
   def parts = mesh.get("parts", [])
   if !is_list(parts) || parts.len == 0 { return [0, 0, 0, 0] }
   mut state = _scene_bounds_accum_new()
   mut subject_count = 0
   mut backdrop_count = 0
   mut i = 0
   while i < parts.len {
      def part = parts.get(i, 0)
      if is_dict(part) && (part.get("visible", true) ? true : false) {
         def pmin = part.get("min", 0)
         def pmax = part.get("max", 0)
         mut wmin = pmin
         mut wmax = pmax
         def model_m = part.get("model", 0)
         if is_list(model_m) && model_m.len >= 16 {
            def wb = _scene_transform_aabb(pmin, pmax, model_m)
            if is_list(wb) && wb.len >= 2 && _scene_bounds_ok(wb.get(0, 0), wb.get(1, 0)) {
               wmin, wmax = wb.get(0, pmin), wb.get(1, pmax)
            }
         }
         if _scene_bounds_ok(wmin, wmax) {
            if _scene_bounds_is_backdrop(wmin, wmax, scene_area_like) || _scene_part_is_backdrop(part, scene_area_like) {
               backdrop_count += 1
            } else {
               state = _scene_bounds_accum(state, wmin, wmax)
               subject_count += 1
            }
         }
      }
      i += 1
   }
   if subject_count <= 0 || backdrop_count <= 0 { return [subject_count, backdrop_count, 0, 0] }
   def bounds = _scene_bounds_accum_result(state)
   [subject_count, backdrop_count, bounds.get(0, 0), bounds.get(1, 0)]
}

fn _scene_part_opts(dict part, f64 scene_area_like, any opts=0) dict {
   mut out = is_dict(opts) ? opts : part.get("opts", 0)
   if !is_dict(out) { out = dict(4) }
   def prim_mode = int(part.get("primitive_mode", 4))
   case prim_mode {
      0 -> {
         out["is_points"] = true
         def has_normals = part.get("has_normals", out.get("has_normals", false)) ? true : false
         if !has_normals { out["unlit"] = true }
      }
      1, 2, 3 -> {
         out["is_lines"] = true
         def has_normals = part.get("has_normals", out.get("has_normals", false)) ? true : false
         if !has_normals { out["unlit"] = true }
      }
      _ -> {}
   }
   if part.get("is_points", false) { out["is_points"] = true }
   if part.get("is_lines", false) { out["is_lines"] = true }
   if part.get("unlit", false) { out["unlit"] = true }
   if part.get("no_cull", false) { out["no_cull"] = true }
   if part.get("double_sided", false) { out["double_sided"] = true }
   if part.get("flip_winding", false) { out["flip_winding"] = true }
   if part.get("index_type_u32", false) { out["index_type_u32"] = true }
   mut force_backdrop = false
   if scene_area_like > 0.0001 {
      def pmin = part.get("min", 0)
      def pmax = part.get("max", 0)
      if is_list(pmin) && is_list(pmax) && pmin.len >= 3 && pmax.len >= 3 {
         mut dx = float(pmax.get(0, 0)) - float(pmin.get(0, 0))
         mut dy = float(pmax.get(1, 0)) - float(pmin.get(1, 0))
         mut dz = float(pmax.get(2, 0)) - float(pmin.get(2, 0))
         if dx < 0 { dx = 0 - dx }
         if dy < 0 { dy = 0 - dy }
         if dz < 0 { dz = 0 - dz }
         mut max_dim = dx
         if dy > max_dim { max_dim = dy }
         if dz > max_dim { max_dim = dz }
         mut min_dim = dx
         if dy < min_dim { min_dim = dy }
         if dz < min_dim { min_dim = dz }
         mut area_like = dx * dy
         def area_xz = dx * dz
         def area_yz = dy * dz
         if area_xz > area_like { area_like = area_xz }
         if area_yz > area_like { area_like = area_yz }
         force_backdrop = max_dim > 0.0001 && min_dim < max_dim * 0.08 && area_like >= scene_area_like * 0.18
      }
   }
   if force_backdrop {
      out["no_cull"] = true
      out["double_sided"] = true
   }
   out
}

fn _scene_part_axis_bounds(any part, bool use_z) any {
   def pmin = part.get("min", 0)
   def pmax = part.get("max", 0)
   if !is_list(pmin)|| !is_list(pmax) || pmin.len < 3 || pmax.len < 3 { return 0 }
   def axis_idx = use_z ? 2 : 0
   def mn = float(pmin.get(axis_idx, 0.0))
   def mx = float(pmax.get(axis_idx, 0.0))
   [mn, mx, (mn + mx) * 0.5]
}

fn _scene_parts_area_like(any parts) f64 {
   if !is_list(parts)|| parts.len == 0 { return 0.0 }
   mut scene_min_x, scene_min_y = 1e9, 1e9
   mut scene_min_z = 1e9
   mut scene_max_x = -1e9
   mut scene_max_y = -1e9
   mut scene_max_z = -1e9
   mut have_bounds = false
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      def part = parts.get(i, 0)
      if is_dict(part) && (part.get("visible", true) ? true : false) {
         def pmin = part.get("min", 0)
         def pmax = part.get("max", 0)
         if is_list(pmin) && is_list(pmax) && pmin.len >= 3 && pmax.len >= 3 {
            def mnx, mny = float(pmin.get(0, 0.0)), float(pmin.get(1, 0.0))
            def mnz = float(pmin.get(2, 0.0))
            def mxx = float(pmax.get(0, 0.0))
            def mxy = float(pmax.get(1, 0.0))
            def mxz = float(pmax.get(2, 0.0))
            if mnx < scene_min_x { scene_min_x = mnx }
            if mny < scene_min_y { scene_min_y = mny }
            if mnz < scene_min_z { scene_min_z = mnz }
            if mxx > scene_max_x { scene_max_x = mxx }
            if mxy > scene_max_y { scene_max_y = mxy }
            if mxz > scene_max_z { scene_max_z = mxz }
            have_bounds = true
         }
      }
      i += 1
   }
   if !have_bounds { return 0.0 }
   def sx, sy = scene_max_x - scene_min_x, scene_max_y - scene_min_y
   def sz = scene_max_z - scene_min_z
   max(sx * sy, max(sx * sz, sy * sz))
}

fn _scene_sheet_foreground_axis_sign(any parts, f64 scene_area_like, bool use_z) int {
   if !is_list(parts)|| parts.len == 0 || scene_area_like <= 0.0001 { return 0 }
   mut backdrop_sum = 0.0
   mut backdrop_weight = 0.0
   mut axis_min = 1e9
   mut axis_max = -1e9
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      def part = parts.get(i, 0)
      if is_dict(part) && (part.get("visible", true) ? true : false) && _scene_part_is_backdrop(part, scene_area_like) {
         def axis = _scene_part_axis_bounds(part, use_z)
         if is_list(axis) {
            def mn, mx = float(axis.get(0, 0.0)), float(axis.get(1, 0.0))
            def ctr = float(axis.get(2, 0.0))
            def wt = _scene_part_front_weight(part)
            backdrop_sum += ctr * wt
            backdrop_weight += wt
            if mn < axis_min { axis_min = mn }
            if mx > axis_max { axis_max = mx }
         }
      }
      i += 1
   }
   if backdrop_weight <= 0.0001 { return 0 }
   def backdrop_ctr = backdrop_sum / backdrop_weight
   def axis_span = max(axis_max - axis_min, 0.0)
   def side_eps = max(0.0025, axis_span * 0.08)
   mut fg_sum = 0.0
   mut fg_weight = 0.0
   mut max_pos = 0.0
   mut max_neg = 0.0
   i = 0
   while i < parts_n {
      def part = parts.get(i, 0)
      if is_dict(part) && (part.get("visible", true) ? true : false) && !_scene_part_is_backdrop(part, scene_area_like) {
         def axis = _scene_part_axis_bounds(part, use_z)
         if is_list(axis) {
            def mn, mx = float(axis.get(0, 0.0)), float(axis.get(1, 0.0))
            def ctr = float(axis.get(2, 0.0))
            def delta = ctr - backdrop_ctr
            def wt = _scene_part_front_weight(part)
            fg_sum += delta * wt
            fg_weight += wt
            def pos_extent = mx - backdrop_ctr
            def neg_extent = backdrop_ctr - mn
            if pos_extent > max_pos { max_pos = pos_extent }
            if neg_extent > max_neg { max_neg = neg_extent }
         }
      }
      i += 1
   }
   if max_pos > max_neg * 1.25 && max_pos > side_eps { return 1 }
   if max_neg > max_pos * 1.25 && max_neg > side_eps { return -1 }
   if fg_weight <= 0.0001 { return 0 }
   def avg_delta = fg_sum / fg_weight
   if avg_delta > side_eps * 0.35 { return 1 }
   if avg_delta < (0.0 - side_eps * 0.35) { return -1 }
   0
}

fn _scene_refresh_bounds_from_parts(any mesh) any {
   if !is_dict(mesh) { return mesh }
   mut parts = mesh.get("parts", 0)
   if !is_list(parts)|| parts.len == 0 { return mesh }
   mut scene_min_x, scene_min_y = 1e9, 1e9
   mut scene_min_z = 1e9
   mut scene_max_x = -1e9
   mut scene_max_y = -1e9
   mut scene_max_z = -1e9
   mut any_bounds = false
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      mut part = parts.get(i, 0)
      if is_dict(part) && (part.get("visible", true) ? true : false) {
         def vptr = part.get("vptr", 0)
         def vcnt = int(part.get("skin_vcnt", part.get("vcnt", 0)))
         mut bounds = _scene_bounds_from_vertex_ptr(vptr, vcnt)
         if !(is_list(bounds) && bounds.len >= 2 && _scene_bounds_ok(bounds.get(0, 0), bounds.get(1, 0))) {
            def old_min = part.get("min", 0)
            def old_max = part.get("max", 0)
            bounds = _scene_bounds_ok(old_min, old_max) ? [old_min, old_max] : 0
         }
         if is_list(bounds) && bounds.len >= 2 {
            def pmin = bounds.get(0, [0.0, 0.0, 0.0])
            def pmax = bounds.get(1, [0.0, 0.0, 0.0])
            mut wmin = pmin
            mut wmax = pmax
            def model_m = part.get("model", 0)
            if is_list(model_m) && model_m.len >= 16 {
               def wb = _scene_transform_aabb(pmin, pmax, model_m)
               if is_list(wb)&& wb.len >= 2 && _scene_bounds_ok(wb.get(0, 0), wb.get(1, 0)) { wmin, wmax = wb.get(0, pmin), wb.get(1, pmax) }
            }
            if _scene_bounds_ok(pmin, pmax) && _scene_bounds_ok(wmin, wmax) {
               part["min"] = pmin
               part["max"] = pmax
               parts[i] = part
               def mnx, mny = _num_or(wmin.get(0, 0.0), 0.0), _num_or(wmin.get(1, 0.0), 0.0)
               def mnz = _num_or(wmin.get(2, 0.0), 0.0)
               def mxx = _num_or(wmax.get(0, 0.0), 0.0)
               def mxy = _num_or(wmax.get(1, 0.0), 0.0)
               def mxz = _num_or(wmax.get(2, 0.0), 0.0)
               if mnx < scene_min_x { scene_min_x = 0.0 + mnx }
               if mny < scene_min_y { scene_min_y = 0.0 + mny }
               if mnz < scene_min_z { scene_min_z = 0.0 + mnz }
               if mxx > scene_max_x { scene_max_x = 0.0 + mxx }
               if mxy > scene_max_y { scene_max_y = 0.0 + mxy }
               if mxz > scene_max_z { scene_max_z = 0.0 + mxz }
               any_bounds = true
            }
         }
      }
      i += 1
   }
   mesh["parts"] = parts
   if any_bounds {
      mesh["min"] = [scene_min_x, scene_min_y, scene_min_z]
      mesh["max"] = [scene_max_x, scene_max_y, scene_max_z]
   }
   mesh
}

fn _scene_refresh_bounds_from_model_parts(any mesh) any {
   "Recomputes scene bounds from each source part's local vertices and model matrix.
   This runs before grouped upload/merge so the camera fit sees the authored
   node transforms instead of a merged or stale local AABB."
   if is_dict(mesh) && _scene_mesh_bool(mesh, "loader_bounds_ready", false) {
      def parts = mesh.get("parts", 0)
      if is_list(parts) && _scene_bounds_ok(mesh.get("min", 0), mesh.get("max", 0)) {
         mut any_hidden = false
         mut i = 0
         while i < parts.len && !any_hidden {
            def part = parts.get(i, 0)
            if is_dict(part)&& !(part.get("visible", true) ? true : false) { any_hidden = true }
            i += 1
         }
         if !any_hidden { return mesh }
      }
   }
   _scene_refresh_bounds_from_parts(mesh)
}

fn _scene_orbit_front_stats(any parts, f64 scene_area_like) list {
   mut face_x, face_z = 0.0, 0.0
   mut face_weight = 0.0
   mut face_count = 0
   mut backdrop_face_x = 0.0
   mut backdrop_face_z = 0.0
   mut backdrop_face_weight = 0.0
   mut backdrop_face_count = 0
   mut has_blend = false
   mut has_backdrop = false
   def part_count = is_list(parts) ? parts.len : 0
   if _scene_many_part_orbit_quick(part_count) {
      mut quick_i = 0
      while quick_i < part_count {
         def part = parts.get(quick_i, 0)
         if is_dict(part) && (part.get("visible", true) ? true : false) {
            if (int(part.get("alpha_u32", 0)) & 3) == 2 { has_blend = true }
            if _scene_part_is_backdrop(part, scene_area_like) { has_backdrop = true }
         }
         quick_i += 1
      }
      return [0.0, 0.0, 0, part_count, has_blend, has_backdrop]
   }
   mut i = 0
   while i < part_count {
      def part = parts.get(i, 0)
      if is_dict(part) && (part.get("visible", true) ? true : false) {
         if (int(part.get("alpha_u32", 0)) & 3) == 2 { has_blend = true }
         def is_backdrop = _scene_part_is_backdrop(part, scene_area_like)
         if is_backdrop { has_backdrop = true }
         def bias = _scene_part_front_bias_xz(part)
         def used = float(bias.get(2, 0.0))
         if used > 0.5 {
            def weight = _scene_part_front_weight(part)
            def norm_x = float(bias.get(0, 0.0)) / used
            def norm_z = float(bias.get(1, 0.0)) / used
            face_x += norm_x * weight
            face_z += norm_z * weight
            face_weight += weight
            face_count += 1
            if is_backdrop {
               backdrop_face_x += norm_x * weight
               backdrop_face_z += norm_z * weight
               backdrop_face_weight += weight
               backdrop_face_count += 1
            }
         }
      }
      i += 1
   }
   if face_weight > 0.0001 {
      face_x /= face_weight
      face_z /= face_weight
   }
   if backdrop_face_weight > 0.0001 {
      face_x, face_z = backdrop_face_x / backdrop_face_weight, backdrop_face_z / backdrop_face_weight
      face_count = backdrop_face_count
   }
   [face_x, face_z, face_count, part_count, has_blend, has_backdrop]
}

fn _scene_refresh_orbit_from_parts(any mesh, bool has_motion) any {
   if !is_dict(mesh) { return mesh }
   def parts = mesh.get("parts", [])
   if !is_list(parts)|| parts.len == 0 { return mesh }
   def bmin = mesh.get("min", [0.0, 0.0, 0.0])
   def bmax = mesh.get("max", [0.0, 0.0, 0.0])
   def mnx = float(bmin.get(0, 0.0))
   def mny = float(bmin.get(1, 0.0))
   def mnz = float(bmin.get(2, 0.0))
   def mxx = float(bmax.get(0, 0.0))
   def mxy = float(bmax.get(1, 0.0))
   def mxz = float(bmax.get(2, 0.0))
   def raw_sx = mxx - mnx
   def raw_sy = mxy - mny
   def raw_sz = mxz - mnz
   def scene_area_like = max(raw_sx * raw_sy, max(raw_sx * raw_sz, raw_sy * raw_sz))
   def orbit_stats = _scene_orbit_front_stats(parts, scene_area_like)
   def orbit_face_x = float(orbit_stats.get(0, 0.0))
   def orbit_face_z = float(orbit_stats.get(1, 0.0))
   def orbit_face_count = int(orbit_stats.get(2, 0))
   def orbit_part_count = int(orbit_stats.get(3, parts.len))
   def orbit_has_blend = bool(orbit_stats.get(4, false))
   def orbit_has_backdrop = bool(orbit_stats.get(5, false))
   def many_part_orbit_quick = _scene_many_part_orbit_quick(orbit_part_count)
   def orbit_pick = _scene_fit_pick_orbit(
      raw_sx, raw_sy, raw_sz,
      raw_sx, raw_sz, 1.0,
      orbit_face_x, orbit_face_z,
      orbit_face_count, orbit_part_count,
      has_motion,
   orbit_has_blend || orbit_has_backdrop)
   mut fit_cam_yaw = _scene_mesh_num(mesh, "fit_cam_yaw", 0.0)
   mut fit_cam_pitch = _scene_mesh_num(mesh, "fit_cam_pitch", 0.0)
   fit_cam_yaw = float(orbit_pick.get(2, fit_cam_yaw))
   def orbit_raw_max = max(raw_sx, max(raw_sy, raw_sz))
   def orbit_raw_min = min(raw_sx, min(raw_sy, raw_sz))
   def orbit_sheet_like = orbit_raw_max > 0.0001 && orbit_raw_min < orbit_raw_max * 0.30
   def orbit_horiz_max = max(raw_sx, raw_sz)
   def orbit_column_like = orbit_horiz_max > 0.0001 && raw_sy > orbit_horiz_max * 1.8
   def orbit_tall_flat_motion = has_motion && orbit_column_like && orbit_raw_max > 0.0001 && orbit_raw_min < orbit_raw_max * 0.55
   def orbit_grid_like = orbit_part_count >= 5 &&
   min(raw_sx, raw_sy) > 0.0001 &&
   raw_sz < min(raw_sx, raw_sy) * 0.55 &&
   abs(raw_sx - raw_sy) < orbit_raw_max * 0.22
   if (orbit_has_backdrop
      || orbit_sheet_like
      || orbit_grid_like
   || orbit_tall_flat_motion)
   && !(orbit_column_like
   && !orbit_tall_flat_motion)
   && !many_part_orbit_quick{
      def orbit_use_z = raw_sz <= raw_sx
      def orbit_fg_sign = _scene_sheet_foreground_axis_sign(parts, scene_area_like, orbit_use_z)
      if orbit_fg_sign != 0 {
         if orbit_use_z {
            fit_cam_yaw = (orbit_fg_sign > 0) ? 0.0 : 180.0
         } else {
            fit_cam_yaw = (orbit_fg_sign > 0) ? -90.0 : 90.0
         }
      }
   }
   if (orbit_has_blend
      || orbit_has_backdrop
      || orbit_sheet_like
      || orbit_grid_like
   || orbit_tall_flat_motion)
   && !(orbit_column_like
      && !orbit_tall_flat_motion){
      fit_cam_pitch = 0.0
   }
   if has_motion
   && !orbit_has_blend
   && !orbit_has_backdrop
   && !orbit_sheet_like
   && !orbit_grid_like
   && !orbit_column_like{
      if abs(fit_cam_yaw) < 0.001 {
         fit_cam_yaw = -35.0
         fit_cam_pitch = -10.0
      }
   }
   mesh["fit_cam_yaw"] = fit_cam_yaw
   mesh["fit_cam_pitch"] = fit_cam_pitch
   mesh
}

fn _scene_solve_fit_camera_pose(
   f64 tcx,
   f64 tcy,
   f64 tcz,
   f64 wsx,
   f64 wsy,
   f64 wsz,
   f64 fit_fov_deg,
   f64 fit_aspect,
   f64 fit_cam_yaw,
   f64 fit_cam_pitch
)  dict {
   def fit_pose = ui_camera.fit_camera_space_pose(tcx, tcy, tcz, wsx, wsy, wsz, fit_fov_deg, fit_aspect, fit_cam_yaw, fit_cam_pitch)
   mut cam_x, cam_y = float(fit_pose.get("x", tcx)), float(fit_pose.get("y", tcy))
   mut cam_z = float(fit_pose.get("z", tcz))
   mut yaw = float(fit_pose.get("yaw", fit_cam_yaw))
   mut pitch = float(fit_pose.get("pitch", fit_cam_pitch))
   if is_nan(cam_x) || is_inf(cam_x) || abs(cam_x) > 1000000.0 ||
   is_nan(cam_y) || is_inf(cam_y) || abs(cam_y) > 1000000.0 ||
   is_nan(cam_z) || is_inf(cam_z) || abs(cam_z) > 1000000.0{
      def fallback_dist = max(1.0, max(wsx, max(wsy, wsz)) * 1.65)
      if is_nan(yaw)|| is_inf(yaw) || abs(yaw) > 360000.0 { yaw = 0.0 }
      if is_nan(pitch)|| is_inf(pitch) || abs(pitch) > 360000.0 { pitch = 0.0 }
      def fallback_yaw = yaw * PI / 180.0
      def fallback_pitch = pitch * PI / 180.0
      def fallback_cp = cos(fallback_pitch)
      def fallback_fx = sin(fallback_yaw) * fallback_cp
      def fallback_fy = sin(fallback_pitch)
      def fallback_fz = 0.0 - cos(fallback_yaw) * fallback_cp
      cam_x, cam_y = tcx - fallback_fx * fallback_dist, tcy - fallback_fy * fallback_dist
      cam_z = tcz - fallback_fz * fallback_dist
   }
   {"x": cam_x, "y": cam_y, "z": cam_z, "yaw": yaw, "pitch": pitch}
}

fn _scene_store_fit_camera(
   dict mesh,
   f64 fit_fov_deg,
   f64 fit_aspect,
   f64 fit_cam_x,
   f64 fit_cam_y,
   f64 fit_cam_z,
   f64 fit_cam_yaw,
   f64 fit_cam_pitch,
   f64 tcx,
   f64 tcy,
   f64 tcz
)  dict {
   mesh["fit_cam_fov"] = fit_fov_deg
   mesh["fit_cam_aspect"] = fit_aspect
   mesh["fit_cam_x"] = fit_cam_x
   mesh["fit_cam_y"] = fit_cam_y
   mesh["fit_cam_z"] = fit_cam_z
   mesh["fit_cam_yaw"] = fit_cam_yaw
   mesh["fit_cam_pitch"] = fit_cam_pitch
   mesh["fit_target_x"] = tcx
   mesh["fit_target_y"] = tcy
   mesh["fit_target_z"] = tcz
   mesh
}

fn _scene_fit_info_num(any info, int idx, f64 fallback) f64 { float(info.get(idx, fallback)) }

fn _scene_mesh_num(dict mesh, str key, f64 fallback=0.0) f64 { float(mesh.get(key, fallback)) }

fn _scene_mesh_int(dict mesh, str key, int fallback=0) int { int(mesh.get(key, fallback)) }

fn _scene_mesh_bool(dict mesh, str key, bool fallback=false) bool { bool(mesh.get(key, fallback)) }

fn _scene_absf(any x) f64 {
   def v = float(x)
   v < 0.0 ? -v : v
}

fn scene_mesh_num(any mesh, str key, f64 fallback=0.0) f64 {
   "Reads a numeric mesh field with a fallback."
   is_dict(mesh) ? float(mesh.get(key, fallback)) : fallback
}

fn scene_mesh_int(any mesh, str key, int fallback=0) int {
   "Reads an integer mesh field with a fallback."
   is_dict(mesh) ? int(mesh.get(key, fallback)) : fallback
}

fn scene_mesh_bool(any mesh, str key, bool fallback=false) bool {
   "Reads a boolean mesh field with a fallback."
   is_dict(mesh) ? bool(mesh.get(key, fallback)) : fallback
}

fn scene_model_baked_for_draw(any mesh) bool {
   "Reports whether a scene mesh is already baked for drawing."
   if !is_dict(mesh) { return false }
   if scene_mesh_bool(mesh, "parts_model_baked", false) { return true }
   def has_gpu_slab = mesh.get("gpu_parts_slab", 0)
   def gpu_state = mesh.get("gpu_draw_state", 0)
   if is_list(gpu_state) && gpu_state.len >= 2 && to_int(gpu_state.get(0, 0)) != 0 && int(gpu_state.get(1, 0)) > 0 {
      if gpu_state.len >= 9 { return int(gpu_state.get(5, 0)) != 0 }
      if gpu_state.len >= 7 { return int(gpu_state.get(4, 0)) != 0 }
   }
   if to_int(has_gpu_slab) != 0 || int(mesh.get("gpu_parts_count", 0)) > 0 {
      return scene_mesh_bool(mesh, "gpu_model_baked", false)
   }
   scene_mesh_bool(mesh, "gpu_model_baked", false)
}

fn scene_edit_scale(any mesh) f64 {
   "Returns the clamped edit scale stored on a scene mesh."
   clamp(scene_mesh_num(mesh, "edit_scale", 1.0), 0.02, 50.0)
}

fn _scene_edit_axis_scale(any mesh, str key, f64 fallback) f64 {
   if is_dict(mesh) && mesh.contains(key) { return clamp(scene_mesh_num(mesh, key, fallback), 0.02, 50.0) }
   clamp(float(fallback), 0.02, 50.0)
}

fn scene_has_edit_transform(any mesh) bool {
   "Reports whether a scene mesh has any edit transform."
   if !is_dict(mesh) { return false }
   def sc = scene_edit_scale(mesh)
   _scene_absf(scene_mesh_num(mesh, "edit_tx", 0.0)) > 0.000001 ||
   _scene_absf(scene_mesh_num(mesh, "edit_ty", 0.0)) > 0.000001 ||
   _scene_absf(scene_mesh_num(mesh, "edit_tz", 0.0)) > 0.000001 ||
   _scene_absf(scene_mesh_num(mesh, "edit_rx", 0.0)) > 0.000001 ||
   _scene_absf(scene_mesh_num(mesh, "edit_ry", 0.0)) > 0.000001 ||
   _scene_absf(scene_mesh_num(mesh, "edit_rz", 0.0)) > 0.000001 ||
   _scene_absf(sc - 1.0) > 0.000001 ||
   _scene_absf(_scene_edit_axis_scale(mesh, "edit_sx", sc) - 1.0) > 0.000001 ||
   _scene_absf(_scene_edit_axis_scale(mesh, "edit_sy", sc) - 1.0) > 0.000001 ||
   _scene_absf(_scene_edit_axis_scale(mesh, "edit_sz", sc) - 1.0) > 0.000001
}

fn scene_edit_transform_matrix_into(any mesh, list mt, list mr, list ms, list out, list tmp) list {
   "Writes the edit transform matrix for a scene mesh."
   def tx = scene_mesh_num(mesh, "edit_tx", 0.0)
   def ty = scene_mesh_num(mesh, "edit_ty", 0.0)
   def tz = scene_mesh_num(mesh, "edit_tz", 0.0)
   def rx = scene_mesh_num(mesh, "edit_rx", 0.0)
   def ry = scene_mesh_num(mesh, "edit_ry", 0.0)
   def rz = scene_mesh_num(mesh, "edit_rz", 0.0)
   def sc = scene_edit_scale(mesh)
   mat4_scale_into(
      _scene_edit_axis_scale(mesh, "edit_sx", sc),
      _scene_edit_axis_scale(mesh, "edit_sy", sc),
      _scene_edit_axis_scale(mesh, "edit_sz", sc),
   ms)
   mat4_rotate_x_into(rx, mr)
   mat4_mul_into(mr, ms, tmp)
   mat4_rotate_y_into(ry, mr)
   mat4_mul_into(mr, tmp, out)
   mat4_rotate_z_into(rz, mr)
   mat4_mul_into(mr, out, tmp)
   mat4_translate_into(tx, ty, tz, mt)
   mat4_mul_into(mt, tmp, out)
   out
}

fn scene_active_model_matrix_into(any mesh, list base, list identity, list out, list mt, list mr, list ms, list edit, list tmp) list {
   "Writes the active model matrix for a scene mesh."
   if !is_dict(mesh) { return base }
   def baked = scene_mesh_bool(mesh, "fit_applied", false) && scene_model_baked_for_draw(mesh)
   def edited = scene_has_edit_transform(mesh)
   if baked && !edited {
      mat4_identity_into(out)
      return out
   }
   if !edited { return base }
   scene_edit_transform_matrix_into(mesh, mt, mr, ms, edit, tmp)
   if baked {
      mat4_mul_into(edit, identity, out)
      return out
   }
   mat4_mul_into(edit, base, out)
   out
}

fn scene_fit_transform_into(any mesh, list base, list mt, list ms) bool {
   "Writes the scene fit transform matrices for a mesh."
   if !is_dict(mesh) { return false }
   if scene_mesh_bool(mesh, "fit_applied", false) && scene_model_baked_for_draw(mesh) {
      mat4_identity_into(mt)
      mat4_identity_into(ms)
      mat4_identity_into(base)
      return true
   }
   def fit_scale = scene_mesh_num(mesh, "fit_scale", 1.0)
   def fit_tx = scene_mesh_num(mesh, "fit_tx", 0.0)
   def fit_ty = scene_mesh_num(mesh, "fit_ty", 0.0)
   def fit_tz = scene_mesh_num(mesh, "fit_tz", 0.0)
   mat4_translate_into(fit_tx, fit_ty, fit_tz, mt)
   mat4_scale_into(fit_scale, fit_scale, fit_scale, ms)
   mat4_mul_into(mt, ms, base)
   true
}

fn scene_drag_begin_state(any scene, any x, any y, any mode=0, any opts=0) dict {
   "Creates drag state for editing a scene transform."
   if !is_dict(scene) { return {"active": false, "ok": false} }
   def px, py = float(x), float(y)
   def options = is_dict(opts) ? opts : dict(0)
   def start_sc = scene_edit_scale(scene)
   mut sax = float(options.get("screen_axis_x", 0.0))
   mut say = float(options.get("screen_axis_y", 0.0))
   def sl = sqrt(sax * sax + say * say)
   if sl > 0.0001 { sax /= sl say /= sl } else { sax = 0.0 say = 0.0 }
   mut axis_wpp = float(options.get("axis_world_per_pixel", 0.0))
   if is_nan(axis_wpp) || is_inf(axis_wpp) || axis_wpp <= 0.000001 { axis_wpp = 0.0 }
   {
      "active": true,
      "ok": true,
      "changed": false,
      "mode": int(mode),
      "axis": int(options.get("axis", 0)),
      "precise": bool(options.get("precise", false)),
      "snap": bool(options.get("snap", false)),
      "screen_axis_x": sax,
      "screen_axis_y": say,
      "axis_world_per_pixel": axis_wpp,
      "drag_world_per_pixel": float(options.get("drag_world_per_pixel", 0.0)),
      "drag_right_x": float(options.get("drag_right_x", 1.0)),
      "drag_right_y": float(options.get("drag_right_y", 0.0)),
      "drag_right_z": float(options.get("drag_right_z", 0.0)),
      "drag_up_x": float(options.get("drag_up_x", 0.0)),
      "drag_up_y": float(options.get("drag_up_y", 1.0)),
      "drag_up_z": float(options.get("drag_up_z", 0.0)),
      "last_x": px,
      "last_y": py,
      "start_x": px,
      "start_y": py,
      "start_tx": scene_mesh_num(scene, "edit_tx", 0.0),
      "start_ty": scene_mesh_num(scene, "edit_ty", 0.0),
      "start_tz": scene_mesh_num(scene, "edit_tz", 0.0),
      "cur_tx": scene_mesh_num(scene, "edit_tx", 0.0),
      "cur_ty": scene_mesh_num(scene, "edit_ty", 0.0),
      "cur_tz": scene_mesh_num(scene, "edit_tz", 0.0),
      "start_rx": scene_mesh_num(scene, "edit_rx", 0.0),
      "start_ry": scene_mesh_num(scene, "edit_ry", 0.0),
      "start_rz": scene_mesh_num(scene, "edit_rz", 0.0),
      "start_scale": start_sc,
      "start_sx": _scene_edit_axis_scale(scene, "edit_sx", start_sc),
      "start_sy": _scene_edit_axis_scale(scene, "edit_sy", start_sc),
      "start_sz": _scene_edit_axis_scale(scene, "edit_sz", start_sc)
   }
}

fn scene_drag_pixel_scale(any bounds, f64 cam_px=0.0, f64 cam_py=0.0, f64 cam_pz=0.0, f64 cam_fov=60.0) f64 {
   "Returns the world-space scale represented by one drag pixel."
   if is_list(bounds) && bounds.len >= 6 {
      def sx = _scene_absf(float(bounds.get(3, 0.0)) - float(bounds.get(0, 0.0)))
      def sy = _scene_absf(float(bounds.get(4, 0.0)) - float(bounds.get(1, 0.0)))
      def sz = _scene_absf(float(bounds.get(5, 0.0)) - float(bounds.get(2, 0.0)))
      def span = max(max(max(sx, sy), sz), 1.0)
      ;; The old linear span scale made large/fit glTF scenes jump by tens of
      ;; units from a tiny mouse move (for example Y instantly landing around
      ;; 74.5).  Use a sub-linear visible-span scale with a hard cap so gizmo
      ;; motion remains predictable across tiny props and huge imported assets.
      def base = clamp(sqrt(span) * 0.0030, 0.0015, 0.0350)
      def cx = (float(bounds.get(0, 0.0)) + float(bounds.get(3, 0.0))) / 2.0
      def cy = (float(bounds.get(1, 0.0)) + float(bounds.get(4, 0.0))) / 2.0
      def cz = (float(bounds.get(2, 0.0)) + float(bounds.get(5, 0.0))) / 2.0
      def dx = cx - cam_px
      def dy = cy - cam_py
      def dz = cz - cam_pz
      mut safe_fov = float(cam_fov)
      if is_nan(safe_fov) || is_inf(safe_fov) || safe_fov < 15.0 || safe_fov > 120.0 { safe_fov = 60.0 }
      def cam_dist = sqrt(dx * dx + dy * dy + dz * dz)
      def fov_rad = safe_fov * (PI / 180.0)
      def raw_ref = span / (2.0 * tan(fov_rad / 2.0) * 0.3)
      def ref_dist = (is_nan(raw_ref) || is_inf(raw_ref) || raw_ref < 0.001) ? 1.0 : raw_ref
      return base * clamp(max(cam_dist, ref_dist * 0.1) / ref_dist, 0.1, 10.0)
   }
   0.010
}

fn _scene_drag_clamp_pixels(any v, f64 limit=72.0) f64 {
   float(v)
}

fn _scene_drag_snap(any value, any step) f64 {
   def s = float(step)
   if s <= 0.000001 { return float(value) }
   floor((float(value) / s) + 0.5) * s
}

fn _scene_drag_axis_delta(f64 dx, f64 dy) f64 {
   _scene_absf(dx) >= _scene_absf(dy) ? dx : (0.0 - dy)
}

fn _scene_drag_axis_fallback_delta(f64 dx, f64 dy, int axis) f64 {
   ;; Emergency fallback only when no projected tangent / ray solve exists.  Y
   ;; must not become dead; screen-up should raise world Y in the common editor
   ;; view, while X/Z keep the old dominant-pixel behavior.
   if axis == 2 { return 0.0 - dy }
   _scene_drag_axis_delta(dx, dy)
}

fn _scene_drag_projected_delta(f64 dx, f64 dy, any st) f64 {
   if is_dict(st) {
      def ax = float(st.get("screen_axis_x", 0.0))
      def ay = float(st.get("screen_axis_y", 0.0))
      def len = sqrt(ax * ax + ay * ay)
      if len > 0.0001 { return dx * (ax / len) + dy * (ay / len) }
   }
   _scene_drag_axis_delta(dx, dy)
}

fn _scene_drag_projected_delta_for_axis(f64 dx, f64 dy, any st, int axis) f64 {
   "Returns stable screen-space drag along the picked gizmo axis."
   if is_dict(st) {
      def ax = float(st.get("screen_axis_x", 0.0))
      def ay = float(st.get("screen_axis_y", 0.0))
      def len = sqrt(ax * ax + ay * ay)
      if len > 0.0001 { return dx * (ax / len) + dy * (ay / len) }
   }
   _scene_drag_axis_fallback_delta(dx, dy, axis)
}

fn _scene_drag_snap_step(any bounds, f64 fallback=0.10) f64 {
   if is_list(bounds) && bounds.len >= 6 {
      def sx = _scene_absf(float(bounds.get(3, 0.0)) - float(bounds.get(0, 0.0)))
      def sy = _scene_absf(float(bounds.get(4, 0.0)) - float(bounds.get(1, 0.0)))
      def sz = _scene_absf(float(bounds.get(5, 0.0)) - float(bounds.get(2, 0.0)))
      return clamp(max(max(sx, sy), sz) * 0.015, 0.05, 2.0)
   }
   fallback
}

fn _scene_drag_bounds_span(any bounds) f64 {
   if is_list(bounds) && bounds.len >= 6 {
      def sx = _scene_absf(float(bounds.get(3, 0.0)) - float(bounds.get(0, 0.0)))
      def sy = _scene_absf(float(bounds.get(4, 0.0)) - float(bounds.get(1, 0.0)))
      def sz = _scene_absf(float(bounds.get(5, 0.0)) - float(bounds.get(2, 0.0)))
      return max(max(max(sx, sy), sz), 1.0)
   }
   1.0
}

fn _scene_drag_world_deadband(any bounds) f64 {
   ;; Tiny numeric noise from ray/plane intersection must not move the object.
   ;; Keep this small so slow precise drags still respond immediately.
   clamp(_scene_drag_bounds_span(bounds) * 0.000020, 0.000002, 0.0025)
}

fn _scene_drag_world_smooth_limit(any bounds) f64 {
   ;; Smooth only micro-steps.  Large intentional mouse moves should remain
   ;; immediate, while sub-pixel ray noise is blended out.
   clamp(_scene_drag_bounds_span(bounds) * 0.0060, 0.0010, 0.35)
}

fn _scene_drag_stabilize_scalar(any st, str key, any bounds, f64 raw) f64 {
   if !is_dict(st) { return raw }
   if is_nan(raw) || is_inf(raw) { return float(st.get(key, 0.0)) }
   def eps = _scene_drag_world_deadband(bounds)
   mut val = _scene_absf(raw) <= eps ? 0.0 : raw
   if !bool(st.get(key + "_ok", false)) {
      st[key] = val
      st[key + "_ok"] = true
      return val
   }
   def prev = float(st.get(key, val))
   def diff = _scene_absf(val - prev)
   mut out = val
   if diff <= eps {
      out = prev
   } elif diff < _scene_drag_world_smooth_limit(bounds) {
      out = prev * 0.35 + val * 0.65
   }
   st[key] = out
   out
}

fn _scene_drag_stabilize_vec(any st, str key, any bounds, f64 raw_x, f64 raw_y, f64 raw_z) list {
   if !is_dict(st) { return [raw_x, raw_y, raw_z] }
   if is_nan(raw_x) || is_inf(raw_x) || is_nan(raw_y) || is_inf(raw_y) || is_nan(raw_z) || is_inf(raw_z) {
      return [float(st.get(key + "_x", 0.0)), float(st.get(key + "_y", 0.0)), float(st.get(key + "_z", 0.0))]
   }
   def eps = _scene_drag_world_deadband(bounds)
   def len = sqrt(raw_x * raw_x + raw_y * raw_y + raw_z * raw_z)
   mut vx, vy = raw_x, raw_y
   mut vz = raw_z
   if len <= eps { vx = 0.0 vy = 0.0 vz = 0.0 }
   if !bool(st.get(key + "_ok", false)) {
      st[key + "_x"] = vx
      st[key + "_y"] = vy
      st[key + "_z"] = vz
      st[key + "_ok"] = true
      return [vx, vy, vz]
   }
   def px = float(st.get(key + "_x", vx))
   def py = float(st.get(key + "_y", vy))
   def pz = float(st.get(key + "_z", vz))
   def step_x, step_y = vx - px, vy - py
   def step_z = vz - pz
   def step = sqrt(step_x * step_x + step_y * step_y + step_z * step_z)
   mut ox, oy = vx, vy
   mut oz = vz
   if step <= eps {
      ox = px
      oy = py
      oz = pz
   } elif step < _scene_drag_world_smooth_limit(bounds) {
      ox = px * 0.35 + vx * 0.65
      oy = py * 0.35 + vy * 0.65
      oz = pz * 0.35 + vz * 0.65
   }
   st[key + "_x"] = ox
   st[key + "_y"] = oy
   st[key + "_z"] = oz
   [ox, oy, oz]
}

fn scene_drag_camera_basis(any yaw_deg, any pitch_deg) dict {
   "Returns a normalized camera right/up/forward basis from editor yaw and pitch."
   def yaw = float(yaw_deg) * (PI / 180.0)
   def pitch = float(pitch_deg) * (PI / 180.0)
   def cp = cos(pitch)
   def fx = sin(yaw) * cp
   def fy = sin(pitch)
   def fz = 0.0 - cos(yaw) * cp
   def rx = cos(yaw)
   def ry = 0.0
   def rz = sin(yaw)
   ;; camera-up = right x forward
   mut ux = ry * fz - rz * fy
   mut uy = rz * fx - rx * fz
   mut uz = rx * fy - ry * fx
   def ul = sqrt(ux * ux + uy * uy + uz * uz)
   if ul > 0.000001 { ux = ux / ul uy = uy / ul uz = uz / ul } else { ux = 0.0 uy = 1.0 uz = 0.0 }
   {"right_x": rx, "right_y": ry, "right_z": rz, "up_x": ux, "up_y": uy, "up_z": uz, "fwd_x": fx, "fwd_y": fy, "fwd_z": fz}
}

fn scene_drag_apply(any scene, any state, any x, any y, any yaw_deg, any bounds=[], f64 cam_px=0.0, f64 cam_py=0.0, f64 cam_pz=0.0, f64 cam_fov=60.0, f64 pitch_deg=0.0) dict {
   "Applies a pointer drag to a scene edit transform."
   mut st = is_dict(state) ? state : dict(0)
   st["ok"] = false
   st["changed"] = false
   if !bool(st.get("active", false)) || !is_dict(scene) { return st }
   def nx, ny = float(x), float(y)
   def frame_dx, frame_dy = nx - float(st.get("last_x", nx)), ny - float(st.get("last_y", ny))
   st["last_x"] = nx
   st["last_y"] = ny
   st["ok"] = true
   if _scene_absf(frame_dx) < 0.001 && _scene_absf(frame_dy) < 0.001 { return st }
   def mul = bool(st.get("precise", false)) ? 0.25 : 1.0
   def dx, dy = (nx - float(st.get("start_x", nx))) * mul, (ny - float(st.get("start_y", ny))) * mul
   def axis = int(st.get("axis", 0))
   def snap = bool(st.get("snap", false))
   case int(st.get("mode", 0)){
      1 -> {
         def rot_step = PI / 12.0
         def amt = _scene_drag_projected_delta_for_axis(dx, dy, st, axis) * 0.005
         mut rx = float(st.get("start_rx", 0.0))
         mut ry = float(st.get("start_ry", 0.0))
         mut rz = float(st.get("start_rz", 0.0))
         if axis == 1 { rx += amt }
         elif axis == 2 { ry += amt }
         elif axis == 3 { rz += amt }
         else {
            rx += dy * 0.004
            ry += dx * 0.004
         }
         scene["edit_rx"] = snap ? _scene_drag_snap(rx, rot_step) : rx
         scene["edit_ry"] = snap ? _scene_drag_snap(ry, rot_step) : ry
         scene["edit_rz"] = snap ? _scene_drag_snap(rz, rot_step) : rz
      }
      2 -> {
         def scale_delta = (axis > 0) ? _scene_drag_projected_delta_for_axis(dx, dy, st, axis) : (dx - dy)
         def factor = exp(scale_delta * 0.0035)
         def scale_step = 0.10
         mut sx = clamp(float(st.get("start_sx", st.get("start_scale", 1.0))) * factor, 0.02, 50.0)
         mut sy = clamp(float(st.get("start_sy", st.get("start_scale", 1.0))) * factor, 0.02, 50.0)
         mut sz = clamp(float(st.get("start_sz", st.get("start_scale", 1.0))) * factor, 0.02, 50.0)
         if axis == 1 {
            sy = float(st.get("start_sy", sy))
            sz = float(st.get("start_sz", sz))
         } elif axis == 2 {
            sx = float(st.get("start_sx", sx))
            sz = float(st.get("start_sz", sz))
         } elif axis == 3 {
            sx = float(st.get("start_sx", sx))
            sy = float(st.get("start_sy", sy))
         }
         if snap {
            sx = clamp(_scene_drag_snap(sx, scale_step), 0.02, 50.0)
            sy = clamp(_scene_drag_snap(sy, scale_step), 0.02, 50.0)
            sz = clamp(_scene_drag_snap(sz, scale_step), 0.02, 50.0)
         }
         scene["edit_sx"] = sx
         scene["edit_sy"] = sy
         scene["edit_sz"] = sz
         scene["edit_scale"] = (sx + sy + sz) / 3.0
      }
      _ -> {
         ;; Translation is intentionally deterministic from mouse-down state.
         ;; Do not mix ray/plane hits, closest-axis solves, and pixel fallback
         ;; during one drag: that made gizmos flicker when the camera angle or
         ;; projected axis was shallow.  The captured basis/tangent maps the 2D
         ;; mouse position into the same camera-space plane for the whole drag.
         mut right_x = float(st.get("drag_right_x", 1.0))
         mut right_y = float(st.get("drag_right_y", 0.0))
         mut right_z = float(st.get("drag_right_z", 0.0))
         mut up_x = float(st.get("drag_up_x", 0.0))
         mut up_y = float(st.get("drag_up_y", 1.0))
         mut up_z = float(st.get("drag_up_z", 0.0))
         def rlen = sqrt(right_x * right_x + right_y * right_y + right_z * right_z)
         if rlen > 0.000001 { right_x = right_x / rlen right_y = right_y / rlen right_z = right_z / rlen } else { right_x = 1.0 right_y = 0.0 right_z = 0.0 }
         def ulen = sqrt(up_x * up_x + up_y * up_y + up_z * up_z)
         if ulen > 0.000001 { up_x = up_x / ulen up_y = up_y / ulen up_z = up_z / ulen } else { up_x = 0.0 up_y = 1.0 up_z = 0.0 }
         mut s = float(st.get("drag_world_per_pixel", 0.0))
         if is_nan(s) || is_inf(s) || s <= 0.000001 { s = scene_drag_pixel_scale(bounds, cam_px, cam_py, cam_pz, cam_fov) }
         s = clamp(s, 0.00001, 10.0)
         def adx = _scene_drag_clamp_pixels(dx)
         def ady = _scene_drag_clamp_pixels(dy)
         mut tx = float(st.get("start_tx", 0.0))
         mut ty = float(st.get("start_ty", 0.0))
         mut tz = float(st.get("start_tz", 0.0))
         if axis > 0 {
            mut axis_delta = 0.0
            mut axis_source = "screen"
            if bool(st.get("axis_world_delta_ok", false)) && bool(st.get("ray_update_ok", true)) {
               axis_delta = float(st.get("axis_world_delta", 0.0))
               axis_source = "ray"
            } else {
               mut axis_scale = float(st.get("axis_world_per_pixel", 0.0))
               if is_nan(axis_scale) || is_inf(axis_scale) || axis_scale <= 0.000001 { axis_scale = s }
               axis_scale = clamp(axis_scale, s * 0.05, s * 64.0)
               axis_delta = _scene_drag_projected_delta_for_axis(adx, ady, st, axis) * axis_scale
            }
            if axis == 1 { tx += axis_delta }
            elif axis == 2 { ty += axis_delta }
            elif axis == 3 { tz += axis_delta }
            st["drag_axis_delta"] = axis_delta
            st["drag_axis_source"] = axis_source
         } else {
            mut vx = (adx * right_x - ady * up_x) * s
            mut vy = (adx * right_y - ady * up_y) * s
            mut vz = (adx * right_z - ady * up_z) * s
            mut vec_source = "camera"
            if bool(st.get("ray_world_delta_ok", false)) && bool(st.get("ray_update_ok", true)) {
               vx = float(st.get("ray_world_dx", vx))
               vy = float(st.get("ray_world_dy", vy))
               vz = float(st.get("ray_world_dz", vz))
               vec_source = "ray"
            }
            tx += vx
            ty += vy
            tz += vz
            st["drag_vec_dx"] = vx
            st["drag_vec_dy"] = vy
            st["drag_vec_dz"] = vz
            st["drag_vec_source"] = vec_source
         }
         if snap {
            def step = _scene_drag_snap_step(bounds)
            tx = _scene_drag_snap(tx, step)
            ty = _scene_drag_snap(ty, step)
            tz = _scene_drag_snap(tz, step)
         }
         st["cur_tx"] = tx
         st["cur_ty"] = ty
         st["cur_tz"] = tz
         scene["edit_tx"] = tx
         scene["edit_ty"] = ty
         scene["edit_tz"] = tz
      }
   }
   st["changed"] = true
   st
}

fn _scene_vec_num(list v, int idx, f64 fallback=0.0) f64 { float(v.get(idx, fallback)) }

fn _scene_fit_transform_from_bounds(dict mesh) list {
   def use_fit = _scene_auto_fit_enabled()
   def bmin, bmax = mesh.get("min",[0.0,0.0,0.0]), mesh.get("max",[0.0,0.0,0.0])
   def mnx, mny = _scene_vec_num(bmin,0,0.0), _scene_vec_num(bmin,1,0.0)
   def mnz=_scene_vec_num(bmin,2,0.0)
   def mxx=_scene_vec_num(bmax,0,0.0)
   def mxy=_scene_vec_num(bmax,1,0.0)
   def mxz=_scene_vec_num(bmax,2,0.0)
   def raw_sx=mxx-mnx
   def raw_sy=mxy-mny
   def raw_sz=mxz-mnz
   def raw_max = max(raw_sx,max(raw_sy,raw_sz))
   def scene_area_like = max(raw_sx * raw_sy, max(raw_sx * raw_sz, raw_sy * raw_sz))
   def fit_sc = (raw_max > 0.0001) ? (72.0 / raw_max) : 1.0
   def fit_tx_sc = 0.0 - (mnx+mxx)*0.5*fit_sc
   def fit_ty_sc = 0.0 - mny*fit_sc
   def fit_tz_sc = 0.0 - (mnz+mxz)*0.5*fit_sc
   def sp_sc = use_fit ? fit_sc : 1.0
   def fit_tx = use_fit ? fit_tx_sc : 0.0
   def fit_ty = use_fit ? fit_ty_sc : 0.0
   def fit_tz = use_fit ? fit_tz_sc : 0.0
   mesh["fit_scale"] = sp_sc
   mesh["fit_tx"] = fit_tx
   mesh["fit_ty"] = fit_ty
   mesh["fit_tz"] = fit_tz
   mesh["fit_world_min"] = [mnx*sp_sc+fit_tx, mny*sp_sc+fit_ty, mnz*sp_sc+fit_tz]
   mesh["fit_world_max"] = [mxx*sp_sc+fit_tx, mxy*sp_sc+fit_ty, mxz*sp_sc+fit_tz]
   def subject_bounds = _scene_subject_bounds_from_parts(mesh, scene_area_like)
   if int(subject_bounds.get(0, 0)) > 0 && int(subject_bounds.get(1, 0)) > 0 {
      def sbmin = subject_bounds.get(2, 0)
      def sbmax = subject_bounds.get(3, 0)
      if _scene_bounds_ok(sbmin, sbmax) {
         mesh["fit_subject_bounds"] = true
         mesh["fit_world_min"] = [
            _scene_vec_num(sbmin, 0, mnx) * sp_sc + fit_tx,
            _scene_vec_num(sbmin, 1, mny) * sp_sc + fit_ty,
            _scene_vec_num(sbmin, 2, mnz) * sp_sc + fit_tz
         ]
         mesh["fit_world_max"] = [
            _scene_vec_num(sbmax, 0, mxx) * sp_sc + fit_tx,
            _scene_vec_num(sbmax, 1, mxy) * sp_sc + fit_ty,
            _scene_vec_num(sbmax, 2, mxz) * sp_sc + fit_tz
         ]
      }
   }
   def wb_min = mesh.get("fit_world_min",[mnx,mny,mnz])
   def wb_max = mesh.get("fit_world_max",[mxx,mxy,mxz])
   def wsx=_scene_vec_num(wb_max,0,mxx)-_scene_vec_num(wb_min,0,mnx)
   def wsy=_scene_vec_num(wb_max,1,mxy)-_scene_vec_num(wb_min,1,mny)
   def wsz=_scene_vec_num(wb_max,2,mxz)-_scene_vec_num(wb_min,2,mnz)
   mut wspan = max(wsx, max(wsy, wsz))
   if wspan < 0.001 { wspan = 10.0 }
   def tcx = (_scene_vec_num(wb_min, 0, mnx) + _scene_vec_num(wb_max, 0, mxx)) * 0.5
   def tcy = (_scene_vec_num(wb_min, 1, mny) + _scene_vec_num(wb_max, 1, mxy)) * 0.5
   def tcz = (_scene_vec_num(wb_min, 2, mnz) + _scene_vec_num(wb_max, 2, mxz)) * 0.5
   [mesh,
      mnx,
      mny,
      mnz,
      mxx,
      mxy,
      mxz,
      raw_sx,
      raw_sy,
      raw_sz,
      sp_sc,
      fit_tx,
      fit_ty,
      fit_tz,
      wsx,
      wsy,
      wsz,
      wspan,
      tcx,
      tcy,
   tcz]
}

fn _scene_log_fit_camera(any label, any fit, any fit_cam_x, any fit_cam_y, any fit_cam_z, any fit_cam_yaw, any fit_cam_pitch) bool {
   if !_gltf_debug_enabled() { return false }
   ui_profile.print_text("[gltf] fit " + label + " bounds min=(" +
      to_str(_scene_fit_info_num(fit, 1, 0.0)) + "," +
      to_str(_scene_fit_info_num(fit, 2, 0.0)) + "," +
      to_str(_scene_fit_info_num(fit, 3, 0.0)) +
      ") max=(" +
      to_str(_scene_fit_info_num(fit, 4, 0.0)) + "," +
      to_str(_scene_fit_info_num(fit, 5, 0.0)) + "," +
      to_str(_scene_fit_info_num(fit, 6, 0.0)) +
      ") scale=" + to_str(_scene_fit_info_num(fit, 10, 1.0)) +
      " tx=" + to_str(_scene_fit_info_num(fit, 11, 0.0)) +
      " ty=" + to_str(_scene_fit_info_num(fit, 12, 0.0)) +
      " tz=" + to_str(_scene_fit_info_num(fit, 13, 0.0)) +
      " cam=(" + to_str(fit_cam_x) + "," + to_str(fit_cam_y) + "," + to_str(fit_cam_z) +
      ") yaw=" + to_str(fit_cam_yaw) +
   " pitch=" + to_str(fit_cam_pitch))
   true
}

fn _scene_store_solved_fit_camera(dict mesh, list fit, f64 fit_fov_deg, f64 fit_aspect, f64 fit_cam_yaw, f64 fit_cam_pitch, str label) list {
   def tcx, tcy = _scene_fit_info_num(fit, 18, 0.0), _scene_fit_info_num(fit, 19, 0.0)
   def tcz = _scene_fit_info_num(fit, 20, 0.0)
   def fit_pose = _scene_solve_fit_camera_pose(tcx, tcy, tcz,
      _scene_fit_info_num(fit, 14, 0.0),
      _scene_fit_info_num(fit, 15, 0.0),
      _scene_fit_info_num(fit, 16, 0.0),
   fit_fov_deg, fit_aspect, fit_cam_yaw, fit_cam_pitch)
   def fit_cam_x, fit_cam_y = _scene_mesh_num(fit_pose, "x", tcx), _scene_mesh_num(fit_pose, "y", tcy)
   def fit_cam_z = _scene_mesh_num(fit_pose, "z", tcz)
   fit_cam_yaw = _scene_mesh_num(fit_pose, "yaw", fit_cam_yaw)
   fit_cam_pitch = _scene_mesh_num(fit_pose, "pitch", fit_cam_pitch)
   _scene_log_fit_camera(label, fit, fit_cam_x, fit_cam_y, fit_cam_z, fit_cam_yaw, fit_cam_pitch)
   [_scene_store_fit_camera(mesh, fit_fov_deg, fit_aspect, fit_cam_x, fit_cam_y, fit_cam_z, fit_cam_yaw, fit_cam_pitch, tcx, tcy, tcz),
   fit_cam_x, fit_cam_y, fit_cam_z, fit_cam_yaw, fit_cam_pitch]
}

fn _scene_recompute_fit_from_bounds(any mesh) any {
   if !is_dict(mesh) { return mesh }
   def fit = _scene_fit_transform_from_bounds(mesh)
   mesh = fit.get(0, mesh)
   mut fit_fov_deg = float(_num_or(mesh.get("fit_cam_fov", 120.0), 120.0))
   if fit_fov_deg <= 1.0 || fit_fov_deg >= 179.0 { fit_fov_deg = 120.0 }
   mut fit_aspect = float(_num_or(mesh.get("fit_cam_aspect", 16.0 / 9.0), 16.0 / 9.0))
   if fit_aspect < 0.1 { fit_aspect = 16.0 / 9.0 }
   mut fit_cam_yaw = float(_num_or(mesh.get("fit_cam_yaw", 0.0), 0.0))
   mut fit_cam_pitch = float(_num_or(mesh.get("fit_cam_pitch", 0.0), 0.0))
   def solved = _scene_store_solved_fit_camera(mesh,
      fit,
      fit_fov_deg,
      fit_aspect,
      fit_cam_yaw,
      fit_cam_pitch,
   "recompute")
   solved.get(0, mesh)
}

fn _scene_fit_pick_orbit(
   f64 raw_sx,
   f64 raw_sy,
   f64 raw_sz,
   f64 wsx,
   f64 wsz,
   f64 dcz,
   f64 avg_face_x,
   f64 avg_face_z,
   int face_count,
   int part_count,
   bool has_motion,
   bool has_blend=false
)  list {
   mut fit_cam_x, fit_cam_z = 0.0, dcz
   mut fit_cam_yaw = 0.0
   def face_len = sqrt(avg_face_x * avg_face_x + avg_face_z * avg_face_z)
   def raw_max = max(raw_sx, max(raw_sy, raw_sz))
   def raw_min = min(raw_sx, min(raw_sy, raw_sz))
   def horiz_max = max(raw_sx, raw_sz)
   def sheet_like = raw_max > 0.0001 && raw_min < raw_max * 0.30
   def column_like = horiz_max > 0.0001 && raw_sy > horiz_max * 1.8
   def tall_flat_motion = has_motion && column_like && raw_max > 0.0001 && raw_min < raw_max * 0.55
   def front_sheet_like = sheet_like || tall_flat_motion
   def grid_like = !has_motion && part_count >= 5 &&
   min(raw_sx, raw_sy) > 0.0001 &&
   raw_sz < min(raw_sx, raw_sy) * 0.55 &&
   abs(raw_sx - raw_sy) < raw_max * 0.22
   if face_count > 0 && face_len > 0.15 {
      def inv_face = 1.0 / face_len
      def front_x = -avg_face_x * inv_face
      def front_z = -avg_face_z * inv_face
      if (has_blend || grid_like) && !column_like {
         if grid_like && !has_motion {
            if raw_sz <= raw_sx {
               fit_cam_x, fit_cam_z = 0.0, dcz
            } else {
               fit_cam_x, fit_cam_z = dcz, 0.0
            }
         } else {
            if abs(front_x) > abs(front_z) {
               if front_x < 0.0 {
                  fit_cam_x = -dcz
               }
               else { fit_cam_x = dcz }
               fit_cam_z = 0.0
            } else {
               fit_cam_x = 0.0
               if front_z < 0.0 {
                  fit_cam_z = -dcz
               }
               else { fit_cam_z = dcz }
            }
         }
         fit_cam_yaw = atan2(-fit_cam_x, fit_cam_z) * 180.0 / PI
         return [fit_cam_x, fit_cam_z, fit_cam_yaw]
      }
      if front_sheet_like {
         if raw_sz <= raw_sx {
            fit_cam_x = 0.0
            if front_z < 0.0 {
               fit_cam_z = -dcz
            }
            else { fit_cam_z = dcz }
         } else {
            if front_x < 0.0 {
               fit_cam_x = -dcz
            }
            else { fit_cam_x = dcz }
            fit_cam_z = 0.0
         }
      } else {
         def right_x, right_z = front_z, -front_x
         mut orbit_x, orbit_z = front_x * 0.82 + right_x * 0.57, front_z * 0.82 + right_z * 0.57
         def orbit_len = sqrt(orbit_x * orbit_x + orbit_z * orbit_z)
         if orbit_len > 0.0001 {
            orbit_x, orbit_z = orbit_x / orbit_len, orbit_z / orbit_len
         } else {
            orbit_x, orbit_z = front_x, front_z
         }
         fit_cam_x, fit_cam_z = orbit_x * dcz, orbit_z * dcz
      }
      fit_cam_yaw = atan2(-fit_cam_x, fit_cam_z) * 180.0 / PI
      return [fit_cam_x, fit_cam_z, fit_cam_yaw]
   }
   if front_sheet_like || (grid_like && !column_like) {
      if raw_sz <= raw_sx {
         fit_cam_x, fit_cam_z = 0.0, dcz
      } else {
         fit_cam_x, fit_cam_z = dcz, 0.0
      }
      fit_cam_yaw = atan2(-fit_cam_x, fit_cam_z) * 180.0 / PI
      return [fit_cam_x, fit_cam_z, fit_cam_yaw]
   }
   def horiz_bias = (wsz > wsx * 1.12) && (wsz > 0.001)
   if horiz_bias {
      if has_motion {
         fit_cam_x, fit_cam_z = dcz * 0.58, dcz * 0.82
         fit_cam_yaw = atan2(-fit_cam_x, fit_cam_z) * 180.0 / PI
      } else {
         fit_cam_x, fit_cam_z = dcz, 0.0
         fit_cam_yaw = -90.0
      }
   } else {
      fit_cam_x, fit_cam_z = dcz * 0.58, dcz * 0.82
      fit_cam_yaw = atan2(-fit_cam_x, fit_cam_z) * 180.0 / PI
   }
   [fit_cam_x, fit_cam_z, fit_cam_yaw]
}

fn _scene_static_pose_upload_part(any part) any {
   if !is_dict(part) { return part }
   mut out = dict_clone(part)
   out["skin_idx"] = -1
   out["skin_bind_vptr"] = 0
   out["skin_joints_ptr"] = 0
   out["skin_weights_ptr"] = 0
   out["skin_runtime_slab"] = 0
   out["dynamic_vertices"] = false
   mut opts = out.get("opts", 0)
   if is_dict(opts) { opts = dict_clone(opts) } else { opts = dict(4) }
   opts["storage"] = "static"
   out["opts"] = opts
   out
}

fn _scene_static_pose_upload_parts(any parts) list {
   mut out = []
   if !is_list(parts) { return out }
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      out = out.append(_scene_static_pose_upload_part(parts.get(i, 0)))
      i += 1
   }
   out
}

fn _scene_upload_static_pose_gpu(any mesh, bool prof_on=false) any {
   if !is_dict(mesh) { return mesh }
   if _scene_mesh_bool(mesh, "static_pose_gpu_ready", false) && mesh.get("gpu_parts_slab", 0) && int(mesh.get("gpu_parts_count", 0)) > 0 {
      return mesh
   }
   if !common.env_enabled("NY_GLTF_STATIC_POSE_GPU") { return mesh }
   if common.env_enabled("NY_GLTF_STATIC_POSE_GPU_OFF") { return mesh }
   def deform_count =
   _scene_mesh_int(mesh, "anim_count", 0) +
   _scene_mesh_int(mesh, "skin_count", 0) +
   _scene_mesh_int(mesh, "morph_target_count", 0)
   if deform_count <= 0 { return mesh }
   if _scene_mesh_bool(mesh, "anim_playing", false) { return mesh }
   def parts = mesh.get("parts", [])
   if !is_list(parts) || parts.len <= 0 { return mesh }
   def t0 = ticks()
   def upload_parts = _scene_static_pose_upload_parts(parts)
   def upload = _upload_scene_gpu_parts(upload_parts)
   if !is_dict(upload) || int(upload.get("upload_ok", 0)) <= 0 {
      mesh["static_pose_gpu_ready"] = false
      _scene_prof_skip(prof_on, "static_pose_gpu", "upload_failed")
      return mesh
   }
   def gpu_parts = upload.get("gpu_parts", [])
   if !is_list(gpu_parts) || gpu_parts.len <= 0 {
      mesh["static_pose_gpu_ready"] = false
      _scene_prof_skip(prof_on, "static_pose_gpu", "empty")
      return mesh
   }
   def scene_lights_count = _scene_mesh_int(mesh, "scene_lights_count", 0)
   def gpu_slab = _pack_scene_gpu_parts_slab(gpu_parts, scene_lights_count <= 0)
   if !gpu_slab {
      mesh["static_pose_gpu_ready"] = false
      _scene_prof_skip(prof_on, "static_pose_gpu", "slab_failed")
      return mesh
   }
   def has_blend = bool(upload.get("has_blend", false))
   def has_optical = bool(upload.get("has_optical", false))
   mesh["gpu_parts"] = gpu_parts
   mesh["gpu_parts_slab"] = gpu_slab
   mesh["gpu_parts_count"] = gpu_parts.len
   mesh["gpu_optical_start"] = int(upload.get("gpu_optical_start", 0))
   mesh["gpu_blend_start"] = int(upload.get("gpu_blend_start", gpu_parts.len))
   mesh["gpu_resources"] = upload.get("gpu_resources", [])
   mesh["has_blend"] = has_blend
   mesh["has_optical"] = has_optical
   mesh["gpu_model_baked"] = false
   mesh["static_pose_gpu_ready"] = true
   mesh["static_pose_parts"] = gpu_parts.len
   mesh["static_pose_vertices"] = int(upload.get("gpu_v_count", 0))
   mesh["static_pose_indices"] = int(upload.get("gpu_i_count", 0))
   mesh["gpu_draw_state"] = [
      gpu_slab, gpu_parts.len,
      int(upload.get("gpu_optical_start", 0)),
      int(upload.get("gpu_blend_start", gpu_parts.len)),
      has_blend ? 1 : 0, 0,
      mesh.get("scene_lights_slab", 0),
      scene_lights_count,
      has_optical ? 1 : 0
   ]
   _scene_prof_elapsed(prof_on, "static_pose_gpu", t0)
   mesh
}

fn scene_apply_fit(any mesh) any {
   "Bakes the stored fit transform into grouped GPU parts so later draws match autofit camera framing."
   if !is_dict(mesh) { return mesh }
   def anim_count = _scene_mesh_int(mesh, "anim_count", 0)
   def skin_count = _scene_mesh_int(mesh, "skin_count", 0)
   def morph_count = _scene_mesh_int(mesh, "morph_target_count", 0)
   if anim_count > 0 {
      def anim_idx = _scene_mesh_int(mesh, "anim_idx", 0)
      mut fit_time = 0.0
      if _scene_mesh_bool(mesh, "anim_time_override", false) { fit_time = _scene_mesh_num(mesh, "anim_time", 0.0) }
      mesh = _scene_apply_gltf_animation(mesh, anim_idx, fit_time)
   }
   if anim_count > 0 || skin_count > 0 || morph_count > 0 {
      mesh = _scene_refresh_bounds_from_parts(mesh)
      mesh = _scene_refresh_orbit_from_parts(mesh, true)
      mesh = _scene_recompute_fit_from_bounds(mesh)
      return _scene_upload_static_pose_gpu(mesh, false)
   }
   if _scene_mesh_bool(mesh, "fit_applied", false) { return mesh }
   if !_scene_mesh_bool(mesh, "gpu_model_baked", false) {
      def cpu_parts = mesh.get("parts", [])
      if is_list(cpu_parts) && cpu_parts.len > 0 && !_scene_mesh_bool(mesh, "parts_model_baked", false) {
         def fit_scale = _scene_mesh_num(mesh, "fit_scale", 1.0)
         def fit_tx = _scene_mesh_num(mesh, "fit_tx", 0.0)
         def fit_ty = _scene_mesh_num(mesh, "fit_ty", 0.0)
         def fit_tz = _scene_mesh_num(mesh, "fit_tz", 0.0)
         def scene_fit_m = _scene_fit_model_mat4(fit_scale, fit_tx, fit_ty, fit_tz)
         def baked_parts = _prebake_scene_render_parts(cpu_parts, scene_fit_m)
         mesh["parts"] = baked_parts
         mesh["parts_count"] = baked_parts.len
         mesh["parts_model_baked"] = true
         mesh["fit_applied"] = true
      }
      return mesh
   }
   def gpu_parts = mesh.get("gpu_parts", [])
   if !is_list(gpu_parts)|| gpu_parts.len == 0 { return mesh }
   def fit_scale = _scene_mesh_num(mesh, "fit_scale", 1.0)
   def fit_tx = _scene_mesh_num(mesh, "fit_tx", 0.0)
   def fit_ty = _scene_mesh_num(mesh, "fit_ty", 0.0)
   def fit_tz = _scene_mesh_num(mesh, "fit_tz", 0.0)
   def scene_fit_m = _scene_fit_model_mat4(fit_scale, fit_tx, fit_ty, fit_tz)
   def baked_parts = _prebake_scene_gpu_parts(gpu_parts, scene_fit_m)
   def baked_slab = _pack_scene_gpu_parts_slab(baked_parts, _scene_mesh_int(mesh, "scene_lights_count", 0) <= 0)
   mut old_baked_slab = mesh.get("gpu_parts_slab", 0)
   def old_gpu_state = mesh.get("gpu_draw_state", 0)
   if is_list(old_gpu_state) && old_gpu_state.len >= 2 && old_gpu_state.get(0, 0) {
      old_baked_slab = old_gpu_state.get(0, old_baked_slab)
   }
   if _gltf_debug_enabled() {
      terminal.log("[gltf] scene_apply_fit: scale=" + to_str(fit_scale) +
         " tx=" + to_str(fit_tx) + " ty=" + to_str(fit_ty) + " tz=" + to_str(fit_tz) +
      " parts=" + to_str(gpu_parts.len))
   }
   if old_baked_slab && old_baked_slab != baked_slab { free(old_baked_slab) }
   mesh["gpu_parts"] = baked_parts
   mesh["gpu_parts_slab"] = baked_slab
   mesh["gpu_parts_count"] = baked_parts.len
   mesh["parts"] = _build_scene_render_parts(baked_parts)
   mesh["parts_count"] = baked_parts.len
   mesh["fit_applied"] = true
   mesh["gpu_draw_state"] = [baked_slab,
      baked_parts.len,
      _scene_mesh_int(mesh, "gpu_optical_start", 0),
      _scene_mesh_int(mesh, "gpu_blend_start", 0),
      _scene_mesh_bool(mesh, "has_blend", false) ? 1 : 0,
      1,
      mesh.get("scene_lights_slab", 0),
      _scene_mesh_int(mesh, "scene_lights_count", 0),
   _scene_mesh_bool(mesh, "has_optical", false) ? 1 : 0]
   mesh
}

fn _scene_try_apply_gltf_camera(
   dict mesh,
   dict gltf_data,
   f64 fit_scale,
   f64 fit_tx,
   f64 fit_ty,
   f64 fit_tz,
   f64 tcx,
   f64 tcy,
   f64 tcz,
   f64 wspan
)  dict {
   if !is_dict(mesh)|| !is_dict(gltf_data) { return mesh }
   if gltf.gltf_camera_count(gltf_data)<= 0 { return mesh }
   def cam_instances = gltf.gltf_camera_instances(gltf_data)
   if !is_list(cam_instances)|| cam_instances.len == 0 { return mesh }
   def inst = cam_instances.get(0, 0)
   if !is_dict(inst) { return mesh }
   def world_m = inst.get("world_matrix", 0)
   if !is_list(world_m)|| world_m.len < 16 { return mesh }
   def cam_info = inst.get("camera", 0)
   def px0 = float(world_m.get(12, 0.0))
   def py0 = float(world_m.get(13, 0.0))
   def pz0 = float(world_m.get(14, 0.0))
   def px = px0 * fit_scale + fit_tx
   def py = py0 * fit_scale + fit_ty
   def pz = pz0 * fit_scale + fit_tz
   def txc, tyc = tcx, tcy
   def tzc = tcz
   mut fx, fy = txc - px, tyc - py
   mut fz = tzc - pz
   def flen = sqrt(fx * fx + fy * fy + fz * fz)
   if flen > 0.000001 {
      fx, fy = fx / flen, fy / flen
      fz = fz / flen
   } else {
      fx, fy = -float(world_m.get(8, 0.0)), -float(world_m.get(9, 0.0))
      fz = -float(world_m.get(10, 1.0))
      def f2 = sqrt(fx * fx + fy * fy + fz * fz)
      if f2 > 0.000001 {
         fx, fy = fx / f2, fy / f2
         fz = fz / f2
      } else {
         fx, fy = 0.0, 0.0
         fz = -1.0
      }
   }
   def horiz = max(0.000001, sqrt(fx*fx + fz*fz))
   def yaw = atan2(fx, -fz) * 180.0 / PI
   def pitch = atan2(fy, horiz) * 180.0 / PI
   mut out = mesh
   out["fit_cam_x"] = px
   out["fit_cam_y"] = py
   out["fit_cam_z"] = pz
   out["fit_cam_yaw"] = yaw
   out["fit_cam_pitch"] = pitch
   out["fit_target_x"] = txc
   out["fit_target_y"] = tyc
   out["fit_target_z"] = tzc
   out["fit_cam_source"] = "gltf_camera"
   out["fit_cam_node_idx"] = int(inst.get("node_idx", -1))
   if is_dict(cam_info) && eq(to_str(cam_info.get("type", "")), "perspective") {
      def yfov = float(cam_info.get("yfov", 0.0))
      if yfov > 0.000001 { out["fit_cam_fov"] = clamp(yfov * 180.0 / PI, 15.0, 120.0) }
   }
   out
}

fn _prebake_scene_gpu_parts(any gpu_parts, any scene_model) any {
   "Bakes the scene-level fit transform into grouped GPU part models once at load time."
   if !is_list(gpu_parts)|| gpu_parts.len == 0 { return gpu_parts }
   def scene_m = _as_render_model_mat4(scene_model)
   mut out = []
   mut i = 0
   def gpu_parts_n = gpu_parts.len
   while i < gpu_parts_n {
      def rec = gpu_parts.get(i, 0)
      if is_list(rec) && rec.len >= 40 {
         mut rec2 = clone(rec)
         def part_m = _as_render_model_mat4(rec.get(10, 0))
         def baked_m = mat4_identity()
         mat4_mul_into(scene_m, part_m, baked_m)
         rec2[10] = baked_m
         out = out.append(rec2)
      } else {
         out = out.append(rec)
      }
      i += 1
   }
   out
}

fn _prebake_scene_render_parts(any parts, any scene_model) any {
   "Bakes the scene-level fit transform into CPU/render-part model matrices."
   if !is_list(parts)|| parts.len == 0 { return parts }
   def scene_m = _as_render_model_mat4(scene_model)
   mut out = []
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      def part = parts.get(i, 0)
      if is_dict(part) {
         mut part2 = part
         def part_m = _as_render_model_mat4(part.get("model", 0))
         def baked_m = mat4_identity()
         mat4_mul_into(scene_m, part_m, baked_m)
         part2["model"] = baked_m
         out = out.append(part2)
      } else {
         out = out.append(part)
      }
      i += 1
   }
   out
}

fn _clone_vertices(any vptr, int vcnt) any {
   if !vptr || vcnt <= 0 { return 0 }
   def out = malloc(vcnt * VERTEX_STRIDE)
   if !out { return 0 }
   memcpy(out, vptr, vcnt * VERTEX_STRIDE)
   out
}

fn _num_or(any v, any d) any {
   if !is_int(v)&& !is_float(v) { return d }
   if is_nan(v)|| is_inf(v) { return d }
   def out = 0.0 + v
   if abs(out)> 1000000.0 { return d }
   out
}

fn _bake_vertex_buffer_model(any vptr, int vcnt, any model_m) bool {
   if !vptr || vcnt <= 0 || !is_list(model_m) { return false }
   def n = model_m.len
   mut base = -1
   if n == 18 && to_str(model_m.get(16, "")) == "mat4" {
      base = 0
   }
   elif (n == 18
      && is_int(model_m.get(0, 0))
      && is_int(model_m.get(1, 0))
      && int(model_m.get(0, 0)) == 4
      && int(model_m.get(1, 0)) == 4){
      base = 2
   }
   if base < 0 { return false }
   def m00, m10 = _num_or(model_m.get(base + 0, 1.0), 1.0), _num_or(model_m.get(base + 1, 0.0), 0.0)
   def m20 = _num_or(model_m.get(base + 2, 0.0), 0.0)
   def m01 = _num_or(model_m.get(base + 4, 0.0), 0.0)
   def m11 = _num_or(model_m.get(base + 5, 1.0), 1.0)
   def m21 = _num_or(model_m.get(base + 6, 0.0), 0.0)
   def m02 = _num_or(model_m.get(base + 8, 0.0), 0.0)
   def m12 = _num_or(model_m.get(base + 9, 0.0), 0.0)
   def m22 = _num_or(model_m.get(base + 10, 1.0), 1.0)
   def m03 = _num_or(model_m.get(base + 12, 0.0), 0.0)
   def m13 = _num_or(model_m.get(base + 13, 0.0), 0.0)
   def m23 = _num_or(model_m.get(base + 14, 0.0), 0.0)
   def det = m00 * (m11 * m22 - m12 * m21) -
   m01 * (m10 * m22 - m12 * m20) +
   m02 * (m10 * m21 - m11 * m20)
   def inv_det = abs(det) > 0.00000001 ? (1.0 / det) : 0.0
   def n00, n01 = (m11 * m22 - m12 * m21) * inv_det, (m12 * m20 - m10 * m22) * inv_det
   def n02 = (m10 * m21 - m11 * m20) * inv_det
   def n10 = (m02 * m21 - m01 * m22) * inv_det
   def n11 = (m00 * m22 - m02 * m20) * inv_det
   def n12 = (m01 * m20 - m00 * m21) * inv_det
   def n20 = (m01 * m12 - m02 * m11) * inv_det
   def n21 = (m02 * m10 - m00 * m12) * inv_det
   def n22 = (m00 * m11 - m01 * m10) * inv_det
   def handedness_sign = det < 0.0 ? -1.0 : 1.0
   mut vi = 0
   while vi < vcnt {
      def off = vptr + vi * VERTEX_STRIDE
      def x = _num_or(load32_f32(off, _VKR_OFF_X), 0.0)
      def y = _num_or(load32_f32(off, _VKR_OFF_Y), 0.0)
      def z = _num_or(load32_f32(off, _VKR_OFF_Z), 0.0)
      store32_f32(off, m00 * x + m01 * y + m02 * z + m03, _VKR_OFF_X)
      store32_f32(off, m10 * x + m11 * y + m12 * z + m13, _VKR_OFF_Y)
      store32_f32(off, m20 * x + m21 * y + m22 * z + m23, _VKR_OFF_Z)
      def nx, ny = _num_or(load32_f32(off, _VKR_OFF_NX), 0.0), _num_or(load32_f32(off, _VKR_OFF_NY), 0.0)
      def nz = _num_or(load32_f32(off, _VKR_OFF_NZ), 1.0)
      mut nnx, nny = n00 * nx + n10 * ny + n20 * nz, n01 * nx + n11 * ny + n21 * nz
      mut nnz = n02 * nx + n12 * ny + n22 * nz
      def nl = sqrt(nnx * nnx + nny * nny + nnz * nnz)
      if nl > 0.000001 { nnx /= nl nny /= nl nnz /= nl }
      store32_f32(off, nnx, _VKR_OFF_NX)
      store32_f32(off, nny, _VKR_OFF_NY)
      store32_f32(off, nnz, _VKR_OFF_NZ)
      def gx, gy = _num_or(load32_f32(off, _VKR_OFF_TX), 0.0), _num_or(load32_f32(off, _VKR_OFF_TY), 0.0)
      def gz, gw = _num_or(load32_f32(off, _VKR_OFF_TZ), 0.0), _num_or(load32_f32(off, _VKR_OFF_TW), 1.0)
      mut hx, hy = n00 * gx + n10 * gy + n20 * gz, n01 * gx + n11 * gy + n21 * gz
      mut hz = n02 * gx + n12 * gy + n22 * gz
      def hl = sqrt(hx * hx + hy * hy + hz * hz)
      if hl > 0.000001 { hx /= hl hy /= hl hz /= hl }
      store32_f32(off, hx, _VKR_OFF_TX)
      store32_f32(off, hy, _VKR_OFF_TY)
      store32_f32(off, hz, _VKR_OFF_TZ)
      store32_f32(off, gw * handedness_sign, _VKR_OFF_TW)
      vi += 1
   }
   true
}

fn _scene_limit_lights(any lights, int max_lights=_SCENE_LIGHT_MAX) list {
   mut out = []
   if !is_list(lights) { return out }
   mut i = 0
   while i < lights.len && i < max_lights {
      out = out.append(lights.get(i, 0))
      i += 1
   }
   out
}

mut _gltf_tex_cache = dict(64)
mut _gltf_tex_resolve_cache = dict(256)

fn _gltf_tex_use_mips() bool {
   if _gltf_tex_mips_cache != -1 { return _gltf_tex_mips_cache == 1 }
   if ui_profile.env_present_cached("NY_TEX_MIPS") {
      _gltf_tex_mips_cache = ui_profile.env_toggle_cached("NY_TEX_MIPS", true) ? 1 : 0
   } else {
      _gltf_tex_mips_cache = common.cached_env_toggle(_gltf_tex_mips_cache, "NY_TEX_MIPS", true)
   }
   _gltf_tex_mips_cache == 1
}

fn _clone_seq_field(any out, str key) any {
   def v = out.get(key, 0)
   if is_list(v)|| is_tuple(v) { out[key] = clone(v) }
   out
}

fn _scene_set_fields(dict out, list fields) dict {
   mut i = 0
   while i < fields.len {
      def f = fields.get(i)
      out[to_str(f.get(0))] = f.get(1, nil)
      i += 1
   }
   out
}

fn _clone_scene_part_cached(any part) any {
   if !is_dict(part) { return part }
   mut out = dict_clone(part)
   out = _clone_seq_field(out, "model")
   out = _clone_seq_field(out, "min")
   out = _clone_seq_field(out, "max")
   def opts = out.get("opts", 0)
   if is_dict(opts) { out["opts"] = dict_clone(opts) }
   out
}

fn _clone_scene_parts_cached(any parts) any {
   if !is_list(parts) { return parts }
   mut out = []
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      out = out.append(_clone_scene_part_cached(parts.get(i, 0)))
      i += 1
   }
   out
}

fn _clone_cached_mesh_dict(dict mesh) dict {
   mut out = dict_clone(mesh)
   def parts = out.get("parts", 0)
   if is_list(parts) { out["parts"] = _clone_scene_parts_cached(parts) }
   out = _clone_seq_field(out, "min")
   out = _clone_seq_field(out, "max")
   out = _clone_seq_field(out, "fit_world_min")
   out = _clone_seq_field(out, "fit_world_max")
   _scene_set_fields(out, [
         ["gpu_parts", []], ["gpu_parts_slab", 0], ["gpu_parts_count", 0],
         ["gpu_optical_start", 0], ["gpu_blend_start", 0], ["gpu_resources", []],
         ["gpu_draw_state", 0], ["scene_lights", []], ["scene_lights_slab", 0],
         ["scene_lights_count", 0],
   ])
}

fn _clone_cached_mesh(any mesh) any {
   if !is_dict(mesh) { return 0 }
   _clone_cached_mesh_dict(mesh)
}

fn _mat_tex_req_key(str base_path, str usage, any uri, int filter=-1, any sampler=0) str {
   def wrap_s = is_dict(sampler) ? int(sampler.get("wrap_s", 10497)) : 10497
   def wrap_t = is_dict(sampler) ? int(sampler.get("wrap_t", 10497)) : 10497
   mut resolved = _resolve_texture_path(uri, base_path)
   if resolved.len <= 0 { resolved = base_path + "|" + str.strip(to_str(uri)) }
   mut key = to_str(usage) + "|"
   key = key + resolved
   key = key + "|f=" + to_str(int(filter))
   key = key + "|ws=" + to_str(wrap_s)
   key = key + "|wt=" + to_str(wrap_t)
   return key
}

fn _mat_sampler_uses_mips(any sampler) bool {
   is_dict(sampler) && bool(sampler.get("min_uses_mips", false))
}

fn _mat_tex_req_load(dict req_map, str key, any uri, str base_path, str usage, int filter=-1, any sampler=0) dict {
   if req_map.contains(key) { return req_map }
   def is_color = (usage == "color" || usage == "emissive")
   def allow_disk_cache = usage != "emissive"
   def format = is_color ? 43 : 37
   def use_mips = _gltf_tex_use_mips() || _mat_sampler_uses_mips(sampler)
   def wrap_s = is_dict(sampler) ? int(sampler.get("wrap_s", 10497)) : 10497
   def wrap_t = is_dict(sampler) ? int(sampler.get("wrap_t", 10497)) : 10497
   mut tex_filter = int(filter)
   if tex_filter < 0 {
      if is_dict(sampler) && (sampler.get("mag_nearest", false) || sampler.get("min_nearest", false)) {
         tex_filter = 0
      }
      else { tex_filter = 1 }
   }
   def resolved = _resolve_texture_path(uri, base_path)
   req_map[key] = {
      "key": key, "uri": to_str(uri), "path": resolved.len > 0 ? resolved : to_str(uri),
      "base_path": base_path, "usage": usage, "format": format, "use_mips": use_mips,
      "allow_disk_cache": allow_disk_cache, "filter": tex_filter,
      "wrap_s": wrap_s, "wrap_t": wrap_t, "sampler": sampler
   }
   return req_map
}

fn _mat_tex_req_load_slot(dict req_map, any spec, str base_path, any slot_key, str request_usage, str texture_usage) dict {
   def slot = to_str(slot_key)
   def uri = to_str(spec.get(slot + "_uri", ""))
   if uri.len == 0 { return req_map }
   def filter = int(spec.get(slot + "_filter", -1))
   def sampler = spec.get(slot + "_sampler", 0)
   def k = _mat_tex_req_key(base_path, request_usage, uri, filter, sampler)
   return _mat_tex_req_load(req_map, k, uri, base_path, texture_usage, filter, sampler)
}

fn _mat_tex_req_load_slots(dict req_map, any spec, str base_path) dict {
   mut out = req_map
   mut i = 0
   def reqs_n = _MAT_TEX_SLOT_REQS.len
   while i < reqs_n {
      def rec = _MAT_TEX_SLOT_REQS.get(i, [])
      out = _mat_tex_req_load_slot(out, spec, base_path, to_str(rec.get(0, "")), to_str(rec.get(1, "")), to_str(rec.get(2, "")))
      i += 1
   }
   return out
}

fn _mat_tex_req_get(dict req_map, str key) int {
   def v = req_map.get(key, -1)
   if is_int(v) { return _stable_loaded_texture_id(int(v)) }
   if is_dict(v) {
      def tid = texture_load_gltf(
         to_str(v.get("uri", v.get("path", ""))),
         to_str(v.get("base_path", "")),
         to_str(v.get("usage", "color")),
         int(v.get("filter", -1)),
         v.get("sampler", 0)
      )
      return _stable_loaded_texture_id(tid)
   }
   return -1
}

fn _mat_tex_upload_key(any spec) str {
   if !is_dict(spec) { return "" }
   def path = to_str(spec.get("path", ""))
   if path.len == 0 { return "" }
   path
   + "|fmt=" + to_str(int(spec.get("format", 37)))
   + "|mips=" + to_str(bool(spec.get("use_mips", false)) ? 1 : 0)
   + "|filter=" + to_str(int(spec.get("filter", -1)))
   + "|ws=" + to_str(int(spec.get("wrap_s", 10497)))
   + "|wt=" + to_str(int(spec.get("wrap_t", 10497)))
}

fn _mat_tex_req_realize(any req_map) any {
   if !is_dict(req_map) { return req_map }
   def trace_on = _gltf_tex_trace_enabled()
   def stage_trace = _scene_stage_trace_enabled()
   mut specs = []
   def ks = keys(req_map)
   mut ki = 0
   def ks_n = ks.len
   while ki < ks_n {
      def k, v = ks.get(ki, ""), req_map.get(k, 0)
      if is_dict(v) {
         def cached_tid = texture_try_load_cached_ex(
            to_str(v.get("path", "")),
            int(v.get("format", 37)),
            bool(v.get("use_mips", false)),
            int(v.get("filter", -1)),
            int(v.get("wrap_s", 10497)),
            int(v.get("wrap_t", 10497))
         )
         if cached_tid >= 0 {
            if trace_on {
               ui_profile.print_text("[gltf:req] cache_only key=" + k + " path=" + to_str(v.get("path",
               "")) + " tid=" + to_str(cached_tid))
            }
            req_map[k] = cached_tid
         } else {
            specs = specs.append(v)
         }
      }
      ki += 1
   }
   _scene_stage(stage_trace, "materials.predecode.realize specs=" + to_str(specs.len) + " cached=" + to_str(ks.len - specs.len))
   if specs.len <= 0 { return req_map }
   def t_decode0 = ticks()
   def decoded_map = _gltf_decode_texture_specs_parallel(specs)
   _scene_stage(stage_trace, "materials.predecode.decoded count=" + to_str(keys(decoded_map).len) +
   " ms=" + to_str(ui_profile.elapsed_ms(t_decode0)))
   def t_upload0 = ticks()
   mut uploaded = dict(max(64, specs.len * 2 + 8))
   mut upload_hits = 0
   mut upload_unique = 0
   mut si = 0
   def specs_n = specs.len
   while si < specs_n {
      def spec = specs.get(si, 0)
      if is_dict(spec) {
         def key = to_str(spec.get("key", ""))
         if key.len > 0 {
            def upload_key = _mat_tex_upload_key(spec)
            mut tid = -1
            if upload_key.len > 0 && uploaded.contains(upload_key) {
               tid = _stable_loaded_texture_id(int(uploaded.get(upload_key, -1)))
               upload_hits += 1
            } else {
               tid = _stable_loaded_texture_id(_upload_decoded_tex(spec, decoded_map))
               if upload_key.len > 0 {
                  uploaded[upload_key] = tid
                  upload_unique += 1
               }
            }
            if trace_on {
               ui_profile.print_text("[gltf:req] key=" + key + " path=" + to_str(spec.get("path",
                        "")) + " fmt=" + to_str(int(spec.get("format",
               37))) + " tid=" + to_str(tid))
            }
            req_map[key] = tid
         }
      }
      si += 1
   }
   _scene_stage(stage_trace, "materials.predecode.uploaded count=" + to_str(specs.len)
      + " unique=" + to_str(upload_unique)
      + " reused=" + to_str(upload_hits)
   + " ms=" + to_str(ui_profile.elapsed_ms(t_upload0)))
   _gltf_free_decoded_texture_map(decoded_map)
   return req_map
}

fn _mat_tex_slot_uri(any spec, any slot_key) str {
   to_str(spec.get(to_str(slot_key) + "_uri", ""))
}

fn _mat_tex_slot_filter(any spec, any slot_key) int {
   int(spec.get(to_str(slot_key) + "_filter", -1))
}

fn _mat_tex_slot_sampler(any spec, any slot_key) any {
   spec.get(to_str(slot_key) + "_sampler", 0)
}

fn _mat_tex_slot_key(any spec, str base_path, any slot_key, str request_usage) str {
   def uri = _mat_tex_slot_uri(spec, slot_key)
   if uri.len == 0 { return "" }
   return _mat_tex_req_key(base_path, request_usage, uri, _mat_tex_slot_filter(spec, slot_key), _mat_tex_slot_sampler(spec, slot_key))
}

fn _mat_tex_load_slot_resolved(
   any tex_req_map,
   bool predecode_active,
   any spec,
   str base_path,
   any slot_key,
   str request_usage,
   str texture_usage,
   bool predecode_fallback=false
) int {
   def uri = _mat_tex_slot_uri(spec, slot_key)
   if uri.len == 0 { return -1 }
   def filter = _mat_tex_slot_filter(spec, slot_key)
   def sampler = _mat_tex_slot_sampler(spec, slot_key)
   if predecode_active {
      def tid = _stable_loaded_texture_id(_mat_tex_req_get(tex_req_map, _mat_tex_req_key(base_path, request_usage, uri, filter, sampler)))
      if tid >= 0 || !predecode_fallback { return tid }
   }
   return _stable_loaded_texture_id(_try_load_tex(uri, base_path, texture_usage, filter, sampler))
}

fn _mat_tex_resolved_slots(any tex_req_map, bool predecode_active, any spec, str base_path) dict {
   mut out = dict(32)
   mut loaded_count = 0
   mut i = 0
   def reqs_n = _MAT_TEX_SLOT_REQS.len
   while i < reqs_n {
      def rec = _MAT_TEX_SLOT_REQS.get(i, [])
      def slot = to_str(rec.get(0, ""))
      def tid = _mat_tex_load_slot_resolved(tex_req_map,
         predecode_active,
         spec,
         base_path,
         slot,
         to_str(rec.get(1, "")),
         to_str(rec.get(2, "")),
      slot == "clearcoat" || slot == "clearcoat_normal")
      out[slot] = tid
      if tid >= 0 { loaded_count += 1 }
      i += 1
   }
   out["_loaded_count"] = loaded_count
   out
}

fn _bake_ext_texture_factors(any spec, str base_path) any {
   if !is_dict(spec) { return spec }
   return spec
}

fn _apply_mat_record_to_part(any part, any mat_records) any {
   if !is_dict(part)|| !is_list(mat_records) { return part }
   mut mi = int(part.get("mat_idx", -1))
   if mi < 0 { mi = int(part.get("material_idx", -1)) }
   if mi < 0 || mi >= mat_records.len { return part }
   def rec = mat_records.get(mi, 0)
   if !is_dict(rec) { return part }
   def part_base_color_u32 = int(part.get("base_color_u32", 0xffffffff))
   def part_material_u32 = int(part.get("material_u32", 0x0000ff00))
   def tex_id = int(rec.get("base", -1))
   def base_color_u32 = int(rec.get("base_color_u32", part_base_color_u32))
   def material_u32 = int(rec.get("material_u32", part_material_u32))
   def normal_tex_id = int(rec.get("normal", -1))
   def normal_tex_word = int(rec.get("normal_tex_word", render_utils.pack_normal_tex_word(normal_tex_id,
            int(rec.get("normal_uv_set",
   0)))))
   def normal_uv_set = int(rec.get("normal_uv_set", 0))
   def occlusion = int(rec.get("occlusion", -1))
   def occlusion_uv_set = int(rec.get("occlusion_uv_set", 0))
   def emissive_tex_id = int(rec.get("emissive", -1))
   def emissive_u32 = int(rec.get("emissive_u32", 0))
   def emissive_uv_set = int(rec.get("emissive_uv_set", 0))
   def alpha_u32 = int(rec.get("alpha_u32", 0))
   def base_uv_xf0 = int(rec.get("base_uv_xf0", 0))
   def base_uv_xf1 = int(rec.get("base_uv_xf1", 0))
   def normal_uv_xf0 = int(rec.get("normal_uv_xf0", 0))
   def normal_uv_xf1 = int(rec.get("normal_uv_xf1", 0))
   def mr_uv_xf0 = int(rec.get("mr_uv_xf0", 0))
   def mr_uv_xf1 = int(rec.get("mr_uv_xf1", 0))
   def occlusion_uv_xf0 = int(rec.get("occlusion_uv_xf0", 0))
   def occlusion_uv_xf1 = int(rec.get("occlusion_uv_xf1", 0))
   def emissive_uv_xf0 = int(rec.get("emissive_uv_xf0", 0))
   def emissive_uv_xf1 = int(rec.get("emissive_uv_xf1", 0))
   def bsdf0_u32 = int(rec.get("bsdf0_u32", 0))
   def bsdf1_u32 = int(rec.get("bsdf1_u32", 0))
   def bsdf2_u32 = int(rec.get("bsdf2_u32", 0))
   def bsdf3_u32 = int(rec.get("bsdf3_u32", 0))
   def bsdf4_u32 = int(rec.get("bsdf4_u32", 0))
   def bsdf5_u32 = int(rec.get("bsdf5_u32", 0))
   def ext2_tex_word = int(rec.get("ext2_tex_word", 0x80000000))
   def bsdf_ext_slab = rec.get("bsdf_ext_slab", 0)
   mut vc_mode = int(part.get("vc_mode", 0))
   if rec.contains("vc_mode") {
      vc_mode = bor(vc_mode, int(rec.get("vc_mode", 0)))
   }
   mut out = part
   out["tex_id"] = tex_id
   out["base_color_u32"] = base_color_u32
   out["material_u32"] = material_u32
   out["normal_tex_id"] = normal_tex_id
   out["normal_tex_word"] = normal_tex_word
   out["normal_uv_set"] = normal_uv_set
   out["occlusion"] = occlusion
   out["occlusion_uv_set"] = occlusion_uv_set
   out["emissive_tex_id"] = emissive_tex_id
   out["emissive_u32"] = emissive_u32
   out["emissive_uv_set"] = emissive_uv_set
   out["alpha_u32"] = alpha_u32
   out["base_uv_xf0"] = base_uv_xf0
   out["base_uv_xf1"] = base_uv_xf1
   out["normal_uv_xf0"] = normal_uv_xf0
   out["normal_uv_xf1"] = normal_uv_xf1
   out["mr_uv_xf0"] = mr_uv_xf0
   out["mr_uv_xf1"] = mr_uv_xf1
   out["occlusion_uv_xf0"] = occlusion_uv_xf0
   out["occlusion_uv_xf1"] = occlusion_uv_xf1
   out["emissive_uv_xf0"] = emissive_uv_xf0
   out["emissive_uv_xf1"] = emissive_uv_xf1
   out["bsdf0_u32"] = bsdf0_u32
   out["bsdf1_u32"] = bsdf1_u32
   out["bsdf2_u32"] = bsdf2_u32
   out["bsdf3_u32"] = bsdf3_u32
   out["bsdf4_u32"] = bsdf4_u32
   out["bsdf5_u32"] = bsdf5_u32
   out["ext2_tex_word"] = ext2_tex_word
   out["bsdf_ext_slab"] = bsdf_ext_slab
   out["vc_mode"] = vc_mode
   out
}

fn _apply_mat_records_to_parts(any parts, any mat_records) any {
   if !is_list(parts)|| !is_list(mat_records) || mat_records.len == 0 { return parts }
   mut out = []
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      out = out.append(_apply_mat_record_to_part(parts.get(i, 0), mat_records))
      i += 1
   }
   out
}

fn _scene_pack_material_word(f64 metallic, f64 roughness, int current_word) int {
   def metallic_u8 = band(int((clamp01(metallic) * 255.0 + 0.5)), 255)
   def rough_u8 = band(int((clamp01(roughness) * 255.0 + 0.5)), 255)
   bor(bor(metallic_u8, bshl(rough_u8, 8)), band(current_word, 0xffff0000))
}

fn _scene_pack_alpha_word(str alpha_mode, f64 alpha_cutoff, f64 occlusion_strength, int current_word) int {
   def mode_code = _mat_alpha_mode_code(str.upper(alpha_mode))
   def cutoff_u8 = band(int((clamp01(alpha_cutoff) * 255.0 + 0.5)), 255)
   def occ_u8 = band(int((clamp01(occlusion_strength) * 255.0 + 0.5)), 255)
   bor(band(current_word, 0xff000000), bor(mode_code, bor(bshl(cutoff_u8, 8), bshl(occ_u8, 16))))
}

fn _scene_normal_tex_id_from_word(int word) int {
   def tid = band(word, 0xffff)
   tid == 0xffff ? -1 : tid
}

fn _scene_normal_scale_from_word(int word) f64 {
   def scale_u7 = band(bshr(word, 24), 127)
   scale_u7 <= 0 ? 1.0 : clamp(float(scale_u7) * 2.0 / 127.0, 0.0, 2.0)
}

fn _scene_pack_normal_word(any rec, f64 normal_scale, bool double_sided) int {
   def current = int(rec.get("normal_tex_word", 0x80000000))
   def normal_id = int(rec.get("normal", _scene_normal_tex_id_from_word(current)))
   def uv_set = int(rec.get("normal_uv_set", (band(current, 0x10000) != 0) ? 1 : 0))
   def clearcoat_only = band(current, 0x80000000) != 0
   def mirrored = band(current, 0x20000) != 0
   render_utils.pack_normal_tex_word(normal_id, uv_set, normal_scale, clearcoat_only, mirrored, double_sided)
}

fn _scene_tweak_dict(any base_color, f64 metallic, f64 roughness) dict {
   if is_dict(base_color) { return base_color }
   {"base_color": base_color, "metallic": metallic, "roughness": roughness}
}

fn _scene_tweak_rgba(dict tweak, any fallback) list {
   def v = tweak.get("base_color", fallback)
   if !is_list(v) { return fallback }
   def f0 = fallback.get(0, 1.0)
   def f1 = fallback.get(1, 1.0)
   def f2 = fallback.get(2, 1.0)
   def f3 = fallback.get(3, 1.0)
   mut out = []
   out = out.append(clamp01(float(v.get(0, f0))))
   out = out.append(clamp01(float(v.get(1, f1))))
   out = out.append(clamp01(float(v.get(2, f2))))
   out.append(clamp01(float(v.get(3, f3))))
}

fn _scene_tweak_vec3(dict tweak, str key, any fallback) list {
   def v = tweak.get(key, fallback)
   if !is_list(v) { return fallback }
   def f0 = fallback.get(0, 0.0)
   def f1 = fallback.get(1, 0.0)
   def f2 = fallback.get(2, 0.0)
   mut out = []
   out = out.append(clamp01(float(v.get(0, f0))))
   out = out.append(clamp01(float(v.get(1, f1))))
   out.append(clamp01(float(v.get(2, f2))))
}

fn _scene_rgba_from_u32(int c) list {
   [
      float(band(c, 255)) / 255.0,
      float(band(bshr(c, 8), 255)) / 255.0,
      float(band(bshr(c, 16), 255)) / 255.0,
      float(band(bshr(c, 24), 255)) / 255.0
   ]
}

fn _scene_raw_material_ref(any scene, int mat_idx) list {
   def gltf_data = is_dict(scene) ? scene.get("gltf_data", 0) : 0
   def gltf_root = is_dict(gltf_data) ? gltf_data.get("gltf", 0) : 0
   def materials = is_dict(gltf_root) ? gltf_root.get("materials", []) : []
   [gltf_data, gltf_root, materials]
}

fn _scene_part_mat_idx(any part) int {
   if !is_dict(part) { return -1 }
   def mi = int(part.get("mat_idx", part.get("material_idx", -1)))
   mi
}

fn _scene_update_material_slab(any slab, int base_color_u32, int material_u32) bool {
   if !slab { return false }
   store32(slab, base_color_u32, 0)
   store32(slab, material_u32, 4)
   true
}

fn _scene_refresh_part_material_slab(any part) any {
   if !is_dict(part) { return part }
   mut out = part
   def new_slab = render_utils.pack_material_slab(out)
   if new_slab {
      def old_slab = out.get("material_slab", 0)
      if old_slab && old_slab != new_slab { free(old_slab) }
      out["material_slab"] = new_slab
      def mesh = out.get("mesh", 0)
      if is_dict(mesh) { mesh["material_slab"] = new_slab }
   }
   out
}

fn _scene_apply_material_tweak_to_part(any part, int mat_idx, dict packed) any {
   if !is_dict(part) || _scene_part_mat_idx(part) != mat_idx { return part }
   mut out = part
   out["base_color_u32"] = int(packed.get("base_color_u32", int(out.get("base_color_u32", 0xffffffff))))
   out["material_u32"] = int(packed.get("material_u32", int(out.get("material_u32", 0x0000ff00))))
   out["alpha_u32"] = int(packed.get("alpha_u32", int(out.get("alpha_u32", 0))))
   out["emissive_u32"] = int(packed.get("emissive_u32", int(out.get("emissive_u32", 0))))
   out["normal_tex_word"] = int(packed.get("normal_tex_word", int(out.get("normal_tex_word", 0x80000000))))
   out["double_sided"] = bool(packed.get("double_sided", bool(out.get("double_sided", false))))
   _scene_refresh_part_material_slab(out)
}

fn _scene_apply_material_tweak_to_parts(any parts, int mat_idx, dict packed) any {
   if !is_list(parts) { return parts }
   mut out = parts
   mut i = 0
   while i < out.len {
      out[i] = _scene_apply_material_tweak_to_part(out.get(i, 0), mat_idx, packed)
      i += 1
   }
   out
}

fn _scene_gpu_rec_mat_idx(any rec) int {
   is_list(rec) ? int(rec.get(47, -1)) : -1
}

fn _scene_apply_material_tweak_to_gpu_parts(any gpu_parts, int mat_idx, dict packed) any {
   if !is_list(gpu_parts) { return gpu_parts }
   mut out = gpu_parts
   mut i = 0
   while i < out.len {
      def rec = out.get(i, 0)
      if is_list(rec) && rec.len > 13 && _scene_gpu_rec_mat_idx(rec) == mat_idx {
         rec[12] = int(packed.get("base_color_u32", int(rec.get(12, 0xffffffff))))
         rec[13] = int(packed.get("material_u32", int(rec.get(13, 0x0000ff00))))
         if rec.len > 15 { rec[15] = int(packed.get("emissive_u32", int(rec.get(15, 0)))) }
         if rec.len > 17 { rec[17] = int(packed.get("alpha_u32", int(rec.get(17, 0)))) }
         if rec.len > 24 { rec[24] = int(packed.get("normal_tex_word", int(rec.get(24, 0x80000000)))) }
         if rec.len > 46 { rec[46] = bool(packed.get("double_sided", int(rec.get(46, 0)) != 0)) ? 1 : 0 }
         out[i] = rec
      }
      i += 1
   }
   out
}

fn _scene_update_raw_material_tweak(any scene, int mat_idx, dict tweak) bool {
   def refs = _scene_raw_material_ref(scene, mat_idx)
   def gltf_data = refs.get(0, 0)
   def gltf_root = refs.get(1, 0)
   def materials = refs.get(2, [])
   if !is_list(materials) || mat_idx < 0 || mat_idx >= materials.len { return false }
   def mat = materials.get(mat_idx, 0)
   if !is_dict(mat) { return false }
   mut pbr = mat.get("pbrMetallicRoughness", 0)
   if !is_dict(pbr) { pbr = dict(4) }
   pbr["baseColorFactor"] = tweak.get("base_color", pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0]))
   pbr["metallicFactor"] = clamp01(float(tweak.get("metallic", pbr.get("metallicFactor", 1.0))))
   pbr["roughnessFactor"] = clamp01(float(tweak.get("roughness", pbr.get("roughnessFactor", 1.0))))
   mat["pbrMetallicRoughness"] = pbr
   mat["alphaMode"] = to_str(tweak.get("alpha_mode", mat.get("alphaMode", "OPAQUE")))
   mat["alphaCutoff"] = clamp01(float(tweak.get("alpha_cutoff", mat.get("alphaCutoff", 0.5))))
   mat["doubleSided"] = bool(tweak.get("double_sided", mat.get("doubleSided", false)))
   mat["emissiveFactor"] = tweak.get("emissive_factor", mat.get("emissiveFactor", [0.0, 0.0, 0.0]))
   mut normal_tex = mat.get("normalTexture", 0)
   if is_dict(normal_tex) {
      normal_tex["scale"] = max(0.0, float(tweak.get("normal_scale", normal_tex.get("scale", 1.0))))
      mat["normalTexture"] = normal_tex
   }
   mut occ_tex = mat.get("occlusionTexture", 0)
   if is_dict(occ_tex) {
      occ_tex["strength"] = clamp01(float(tweak.get("occlusion_strength", occ_tex.get("strength", 1.0))))
      mat["occlusionTexture"] = occ_tex
   }
   mut ext = mat.get("extensions", 0)
   if !is_dict(ext) { ext = dict(2) }
   mut ext_es = ext.get("KHR_materials_emissive_strength", 0)
   if !is_dict(ext_es) { ext_es = dict(1) }
   ext_es["emissiveStrength"] = max(0.0, float(tweak.get("emissive_strength", ext_es.get("emissiveStrength", 1.0))))
   ext["KHR_materials_emissive_strength"] = ext_es
   mat["extensions"] = ext
   materials[mat_idx] = mat
   gltf_root["materials"] = materials
   if is_dict(gltf_data) { gltf_data["gltf"] = gltf_root }
   scene["gltf_data"] = gltf_data
   true
}

fn scene_apply_material_tweak(any scene, int mat_idx, any base_color, f64 metallic, f64 roughness) any {
   "Applies a material override to one loaded material."
   if !is_dict(scene) { return scene }
   mut out = scene
   mut records = out.get("mat_records", [])
   if !is_list(records) || mat_idx < 0 || mat_idx >= records.len { return out }
   mut rec = records.get(mat_idx, 0)
   if !is_dict(rec) { return out }
   mut tweak = _scene_tweak_dict(base_color, metallic, roughness)
   def base_rgba = _scene_tweak_rgba(tweak, _scene_rgba_from_u32(int(rec.get("base_color_u32", 0xffffffff))))
   def metallic_v = clamp01(float(tweak.get("metallic", float(band(int(rec.get("material_u32", 0x0000ff00)), 255)) / 255.0)))
   def roughness_v = clamp01(float(tweak.get("roughness", float(band(bshr(int(rec.get("material_u32", 0x0000ff00)), 8), 255)) / 255.0)))
   def alpha_word0 = int(rec.get("alpha_u32", 0))
   def alpha_mode = to_str(tweak.get("alpha_mode", (band(alpha_word0, 3) == 1) ? "MASK" : ((band(alpha_word0, 3) == 2) ? "BLEND" : "OPAQUE")))
   def alpha_cutoff = clamp01(float(tweak.get("alpha_cutoff", float(band(bshr(alpha_word0, 8), 255)) / 255.0)))
   def occlusion_strength = clamp01(float(tweak.get("occlusion_strength", float(band(bshr(alpha_word0, 16), 255)) / 255.0)))
   def emissive_factor = _scene_tweak_vec3(tweak, "emissive_factor", rec.get("emissive_factor", [0.0, 0.0, 0.0]))
   def emissive_strength = max(0.0, float(tweak.get("emissive_strength", rec.get("emissive_strength", 1.0))))
   def normal_scale = max(0.0, float(tweak.get("normal_scale", rec.get("normal_scale", _scene_normal_scale_from_word(int(rec.get("normal_tex_word", 0x80000000)))))))
   def double_sided = bool(tweak.get("double_sided", rec.get("double_sided", false)))
   def base_color_u32 = _mat_pack_base_color_u32(base_rgba)
   def material_u32 = _scene_pack_material_word(metallic_v, roughness_v, int(rec.get("material_u32", 0x0000ff00)))
   def alpha_u32 = _scene_pack_alpha_word(alpha_mode, alpha_cutoff, occlusion_strength, alpha_word0)
   def emissive_u32 = render_utils.pack_emissive_u32(emissive_factor, emissive_strength)
   def normal_tex_word = _scene_pack_normal_word(rec, normal_scale, double_sided)
   rec["base_color_u32"] = base_color_u32
   rec["material_u32"] = material_u32
   rec["alpha_u32"] = alpha_u32
   rec["emissive_factor"] = emissive_factor
   rec["emissive_strength"] = emissive_strength
   rec["emissive_u32"] = emissive_u32
   rec["normal_scale"] = normal_scale
   rec["normal_tex_word"] = normal_tex_word
   rec["double_sided"] = double_sided
   rec["material_tweak"] = true
   rec["metallic_factor"] = metallic_v
   rec["roughness_factor"] = roughness_v
   records[mat_idx] = rec
   out["mat_records"] = records
   def packed = {
      "base_color_u32": base_color_u32, "material_u32": material_u32,
      "alpha_u32": alpha_u32, "emissive_u32": emissive_u32,
      "normal_tex_word": normal_tex_word, "double_sided": double_sided
   }
   out["parts"] = _scene_apply_material_tweak_to_parts(out.get("parts", []), mat_idx, packed)
   mut gpu_parts = _scene_apply_material_tweak_to_gpu_parts(out.get("gpu_parts", []), mat_idx, packed)
   if is_list(gpu_parts) && gpu_parts.len > 0 {
      out["gpu_parts"] = gpu_parts
      def new_slab = _pack_scene_gpu_parts_slab(gpu_parts, int(out.get("scene_lights_count", 0)) <= 0)
      if new_slab {
         def old_slab = out.get("gpu_parts_slab", 0)
         if old_slab && old_slab != new_slab { free(old_slab) }
         out["gpu_parts_slab"] = new_slab
         out["gpu_parts_count"] = gpu_parts.len
         mut draw_state = out.get("gpu_draw_state", [])
         if is_list(draw_state) && draw_state.len >= 2 {
            draw_state[0] = new_slab
            draw_state[1] = gpu_parts.len
            out["gpu_draw_state"] = draw_state
         }
      }
   }
   tweak["base_color"] = base_rgba
   tweak["metallic"] = metallic_v
   tweak["roughness"] = roughness_v
   tweak["alpha_mode"] = alpha_mode
   tweak["alpha_cutoff"] = alpha_cutoff
   tweak["occlusion_strength"] = occlusion_strength
   tweak["emissive_factor"] = emissive_factor
   tweak["emissive_strength"] = emissive_strength
   tweak["normal_scale"] = normal_scale
   tweak["double_sided"] = double_sided
   _scene_update_raw_material_tweak(out, mat_idx, tweak)
   out["material_tweak_revision"] = int(out.get("material_tweak_revision", 0)) + 1
   out
}

fn _load_ogeom_cache(any gltf_path, any mat_records) any {
   if !is_str(gltf_path)|| gltf_path.len == 0 { return 0 }
   _scene_ensure_runtime_caches()
   def mu = _gltf_ogeom_cache_mutex()
   if mu { mutex_lock(mu) }
   def cached = _gltf_ogeom_cache.get(gltf_path, 0)
   if mu { mutex_unlock(mu) }
   if !is_dict(cached) { return 0 }
   _clone_cached_mesh(cached)
}

fn _save_ogeom_cache(any gltf_path, any mesh) int {
   if !is_str(gltf_path)|| gltf_path.len == 0 || !is_dict(mesh) { return 0 }
   _scene_ensure_runtime_caches()
   mut cached = _clone_cached_mesh_dict(mesh)
   cached["cached"] = true
   def mu = _gltf_ogeom_cache_mutex()
   if mu { mutex_lock(mu) }
   if !_gltf_ogeom_cache.contains(gltf_path) { _gltf_ogeom_cache_order = _gltf_ogeom_cache_order.append(gltf_path) }
   _gltf_ogeom_cache[gltf_path] = cached
   while _gltf_ogeom_cache_order.len > _GLTF_OGEOM_CACHE_LIMIT {
      def drop = to_str(_gltf_ogeom_cache_order.get(0, ""))
      _gltf_ogeom_cache_order = slice(_gltf_ogeom_cache_order, 1, _gltf_ogeom_cache_order.len, 1)
      if drop.len > 0 { _gltf_ogeom_cache = _gltf_ogeom_cache.delete(drop) }
   }
   if mu { mutex_unlock(mu) }
   1
}

fn _try_alt_texture_path(any path) str {
   "Tries common fallback extensions for assets that reference unsupported containers(e.g. KTX2)."
   if !is_str(path)|| path.len == 0 { return "" }
   def stem = to_str(ospath.splitext(path).get(0, ""))
   if stem.len == 0 { return "" }
   def p_png = stem + ".png"
   if osfs.is_file(p_png) { return p_png }
   def p_jpg = stem + ".jpg"
   if osfs.is_file(p_jpg) { return p_jpg }
   def p_jpeg = stem + ".jpeg"
   if osfs.is_file(p_jpeg) { return p_jpeg }
   def p_webp = stem + ".webp"
   if osfs.is_file(p_webp) { return p_webp }
   ""
}

fn _resolve_texture_path(any uri, str base_path) str {
   _scene_ensure_runtime_caches()
   def p = str.strip(to_str(uri))
   if p.len == 0 { return "" }
   def cache_key = base_path + "|" + p
   if _gltf_tex_resolve_cache.contains(cache_key) { return to_str(_gltf_tex_resolve_cache.get(cache_key, "")) }
   mut resolved = p
   if !ospath.is_abs(resolved)&& base_path.len > 0 && !str.startswith(resolved, base_path + "/") { resolved = ospath.join(base_path, resolved) }
   if base_path.len > 0 {
      def doubled = base_path + "/" + base_path + "/"
      if str.startswith(resolved, doubled) { resolved = str.str_slice(resolved, base_path.len + 1, resolved.len) }
   }
   if osfs.is_file(resolved) {
      _gltf_tex_resolve_cache[cache_key] = resolved
      return resolved
   }
   def alt = _try_alt_texture_path(resolved)
   if alt.len > 0 {
      _gltf_tex_resolve_cache[cache_key] = alt
      return alt
   }
   _gltf_tex_resolve_cache[cache_key] = resolved
   resolved
}

fn _tex_decode_worker(any spec) any {
   if !is_dict(spec) { return 0 }
   def path = to_str(spec.get("path", ""))
   if path.len == 0 { return 0 }
   def img = lib_img.load(path)
   if !img { return 0 }
   mut out = dict(4)
   out["path"] = path
   out["img"] = img
   out
}

fn _gltf_decode_texture_batch(any specs) dict {
   if !is_list(specs) { return dict(0) }
   mut decoded = dict(max(64, specs.len * 2 + 8))
   mut i = 0
   def n = specs.len
   while i < n {
      def spec = specs.get(i, 0)
      def dec = _tex_decode_worker(spec)
      if is_dict(dec) {
         def path = to_str(dec.get("path", ""))
         def img = dec.get("img", 0)
         if path.len > 0 && img { decoded[path] = img }
      }
      i += 1
   }
   decoded
}

fn _gltf_decode_texture_specs_parallel(any specs) dict {
   if !is_list(specs) { return dict(0) }
   mut unique_specs = []
   mut seen_paths = dict(max(64, specs.len * 2 + 8))
   mut ui = 0
   def specs_n = specs.len
   while ui < specs_n {
      def spec = specs.get(ui, 0)
      if is_dict(spec) {
         def path = to_str(spec.get("path", ""))
         if path.len > 0 && !seen_paths.contains(path) {
            seen_paths[path] = true
            unique_specs = unique_specs.append(spec)
         }
      }
      ui += 1
   }
   def total = unique_specs.len
   if total <= 0 { return dict(0) }
   def want_env = _gltf_decode_threads()
   mut worker_count = 1
   if want_env > 0 { worker_count = want_env }
   if worker_count > total { worker_count = total }
   if worker_count <= 1 { return _gltf_decode_texture_batch(unique_specs) }
   def chunk = int((total + worker_count - 1) / worker_count)
   mut handles = []
   mut wi = 0
   while wi < worker_count {
      def start = wi * chunk
      if start >= total { break }
      mut end = start + chunk
      if end > total { end = total }
      def batch = slice(unique_specs, start, end, 1)
      def h = thread_spawn(fn() {
            _gltf_decode_texture_batch(batch)
      })
      if h { handles = handles.append(h) }
      wi += 1
   }
   mut decoded = dict(max(64, total * 2 + 8))
   mut hi = 0
   def handles_n = handles.len
   while hi < handles_n {
      def res = thread_join(handles.get(hi, 0))
      if is_dict(res) {
         def ks = keys(res)
         mut ki = 0
         def ks_n = ks.len
         while ki < ks_n {
            def k = ks.get(ki, "")
            if len(to_str(k)) > 0 { decoded[to_str(k)] = res.get(k, 0) }
            ki += 1
         }
      }
      hi += 1
   }
   decoded
}

fn _gltf_predecode_batch_enabled(int mat_count=0) bool {
   common.env_toggle("NY_GLTF_PREDECODE_BATCH", mat_count >= 4)
}

fn _gltf_free_decoded_texture_map(any decoded_map) bool {
   if !is_dict(decoded_map) { return false }
   def ks = keys(decoded_map)
   mut ki = 0
   def ks_n = ks.len
   while ki < ks_n {
      def k = ks.get(ki, "")
      def img = decoded_map.get(k, 0)
      if img { lib_img.free(img) }
      ki += 1
   }
   true
}

fn _upload_decoded_tex(any spec, any decoded_map) int {
   if !is_dict(spec) { return -1 }
   def path = to_str(spec.get("path", ""))
   def upload_trace = _scene_tex_upload_trace_enabled()
   def label = upload_trace ? (path.len > 0 ? ospath.basename(path) : to_str(spec.get("uri", ""))) : ""
   def img = decoded_map.get(path, 0)
   if img {
      def t_direct0 = upload_trace ? ticks() : 0
      def tex_id = _stable_loaded_texture_id(texture_upload_image_ex(img,
            path,
            int(spec.get("format",
            37)),
            bool(spec.get("use_mips",
            false)),
            bool(spec.get("allow_disk_cache",
            true)),
            int(spec.get("filter",
            -1)),
            int(spec.get("wrap_s",
            10497)),
            int(spec.get("wrap_t",
            10497)),
            "",
      false))
      _scene_stage(upload_trace, "materials.tex_upload mode=decoded file=" + label +
         " tex=" + to_str(tex_id) +
      " ms=" + to_str(ui_profile.elapsed_ms(t_direct0)))
      if tex_id >= 0 { return tex_id }
      if _gltf_tex_trace_enabled() { ui_profile.print_text("[gltf:req] direct_upload_fail path=" + path + " fmt=" + to_str(int(spec.get("format", 37)))) }
   }
   if _gltf_tex_trace_enabled() { ui_profile.print_text("[gltf:req] fallback_load path=" + path + " uri=" + to_str(spec.get("uri", path))) }
   def t_fallback0 = upload_trace ? ticks() : 0
   def fallback_tid = _stable_loaded_texture_id(texture_load_gltf(to_str(spec.get("uri",
         path)),
         to_str(spec.get("base_path",
         "")),
         to_str(spec.get("usage",
         "color")),
         int(spec.get("filter",
         -1)),
         spec.get("sampler",
   0)))
   _scene_stage(upload_trace, "materials.tex_upload mode=fallback file=" + label +
      " tex=" + to_str(fallback_tid) +
   " ms=" + to_str(ui_profile.elapsed_ms(t_fallback0)))
   return fallback_tid
}

fn _try_load_tex(any uri, str base_path, str usage="color", int filter=-1, any sampler=0) int {
   def t0 = _gltf_tex_trace_enabled() ? ticks() : 0
   def p = str.strip(to_str(uri))
   if p.len == 0 { return -1 }
   if str.find(p, "data:") == 0 {
      def direct = _stable_loaded_texture_id(texture_load_gltf(p, "", usage, filter, sampler))
      if _gltf_tex_trace_enabled() {
         print(
            "[gltf:tex] usage=" + usage
            + " uri=" + p
            + " filter=" + to_str(int(filter))
            + " tex=" + to_str(direct)
            + " ms=" + to_str(ui_profile.elapsed_ms(t0))
         )
      }
      return direct
   }
   _scene_ensure_runtime_caches()
   mut resolved = _resolve_texture_path(p, base_path)
   def wrap_s = is_dict(sampler) ? int(sampler.get("wrap_s", 10497)) : 10497
   def wrap_t = is_dict(sampler) ? int(sampler.get("wrap_t", 10497)) : 10497
   if resolved.len <= 0 { resolved = base_path + "|" + p }
   mut key = to_str(usage) + "|"
   key = key + resolved
   key = key + "|f=" + to_str(int(filter))
   key = key + "|ws=" + to_str(wrap_s)
   key = key + "|wt=" + to_str(wrap_t)
   if _gltf_tex_cache.contains(key) {
      def cached = int(_gltf_tex_cache.get(key, -1))
      return cached
   }
   mut tid = _stable_loaded_texture_id(texture_load_gltf(resolved.len > 0 ? resolved : uri, "", usage, filter, sampler))
   if tid < 0 {
      def alt = _try_alt_texture_path(resolved)
      if alt.len > 0 {
         tid = _stable_loaded_texture_id(texture_load_gltf(alt, "", usage, filter, sampler))
         if tid >= 0 { resolved = alt }
      }
   }
   if tid < 0 && _scene_diag_enabled() { terminal.log("[gltf:tex] failed usage=" + usage + " uri=" + p + " base=" + base_path) }
   if _gltf_tex_trace_enabled() {
      print(
         "[gltf:tex] usage=" + usage
         + " uri=" + p
         + " filter=" + to_str(int(filter))
         + " tex=" + to_str(tid)
         + " ms=" + to_str(ui_profile.elapsed_ms(t0))
      )
   }
   _gltf_tex_cache[key] = tid
   return tid
}

fn _scene_texcoord_one(any spec, str key) int {
   int(spec.get(key, 0)) == 1 ? 1 : 0
}

fn _mat_uv_xf_same(any a, any b) bool {
   int(a.get(0, 0)) == int(b.get(0, 0)) && int(a.get(1, 0)) == int(b.get(1, 0))
}

fn _mat_ext2_try_tex(int ext2_kind, int ext2_id, int ext2_uv_set, list mr_uv_xf, int tex_id, int kind, dict spec, str texcoord_key, list tex_uv_xf, bool mr_has_real, int met_rough_uv_set) list {
   if int(ext2_id) >= 0 || int(tex_id) < 0 { return [ext2_kind, ext2_id, ext2_uv_set, mr_uv_xf] }
   def cand_uv_set = _scene_texcoord_one(spec, texcoord_key)
   def can_share = !mr_has_real || (int(met_rough_uv_set) == cand_uv_set && _mat_uv_xf_same(mr_uv_xf, tex_uv_xf))
   if !can_share { return [ext2_kind, ext2_id, ext2_uv_set, mr_uv_xf] }
   [kind, tex_id, cand_uv_set, mr_has_real ? mr_uv_xf : tex_uv_xf]
}

fn _mat_ext2_try_diffuse_transmission(int ext2_kind, int ext2_id, int ext2_uv_set, list mr_uv_xf, int diffuse_id, int diffuse_color_id, dict spec, list diffuse_uv_xf, list diffuse_color_uv_xf, bool mr_has_real, int met_rough_uv_set) list {
   if int(ext2_id) >= 0 || int(diffuse_id) < 0 { return [ext2_kind, ext2_id, ext2_uv_set, mr_uv_xf] }
   def cand_uv_set = _scene_texcoord_one(spec, "diffuse_transmission_texcoord")
   def same_color_map = int(diffuse_color_id) >= 0 &&
   int(diffuse_color_id) == int(diffuse_id) &&
   int(spec.get("diffuse_transmission_color_texcoord", 0)) == cand_uv_set &&
   _mat_uv_xf_same(diffuse_uv_xf, diffuse_color_uv_xf)
   if !same_color_map { return [ext2_kind, ext2_id, ext2_uv_set, mr_uv_xf] }
   def can_share = !mr_has_real || (int(met_rough_uv_set) == cand_uv_set && _mat_uv_xf_same(mr_uv_xf, diffuse_uv_xf))
   if !can_share { return [ext2_kind, ext2_id, ext2_uv_set, mr_uv_xf] }
   [6, diffuse_id, cand_uv_set, mr_has_real ? mr_uv_xf : diffuse_uv_xf]
}

fn _mat_ext2_try_anisotropy_factor(int ext2_kind, int ext2_id, int ext2_uv_set, dict spec) list {
   if int(ext2_id) >= 0 { return [ext2_kind, ext2_id, ext2_uv_set] }
   def aniso_strength_live = float(spec.get("anisotropy_strength", 0.0))
   def aniso_rotation_live = float(spec.get("anisotropy_rotation", 0.0))
   if aniso_strength_live > 0.000001 || abs(aniso_rotation_live) > 0.000001 { return [4, 0xffff, 0] }
   [ext2_kind, ext2_id, ext2_uv_set]
}

fn _mat_pack_ext2_tex_word(int ext2_kind, int ext2_id, int ext2_uv_set, dict spec) int {
   if int(ext2_id) < 0 { return 0x80000000 }
   mut ext2_tex_word = band(int(ext2_id), 0xffff)
   if int(ext2_uv_set) == 1 { ext2_tex_word = bor(ext2_tex_word, 0x10000) }
   if int(ext2_kind) == 4 {
      mut aniso_rot = float(spec.get("anisotropy_rotation", 0.0))
      while aniso_rot <= -3.141592653589793 { aniso_rot += 6.283185307179586 }
      while aniso_rot > 3.141592653589793 { aniso_rot -= 6.283185307179586 }
      mut rot_bits = 64
      if abs(aniso_rot)> 0.000001 { rot_bits = int((clamp01((aniso_rot + 3.141592653589793) / 6.283185307179586) * 127.0 + 0.5)) }
      ext2_tex_word = bor(ext2_tex_word, bshl(band(rot_bits, 127), 17))
   }
   bor(ext2_tex_word, bshl(int(ext2_kind), 24))
}

fn _mat_apply_diffuse_transmission_color_fallback(dict spec, f64 diffuse_transmission_factor_live) dict {
   if !is_dict(spec) { return spec }
   if float(diffuse_transmission_factor_live) <= 0.000001 { return spec }
   if float(spec.get("subsurface_factor", 0.0)) <= 0.000001 { return spec }
   def dtc_live = spec.get("diffuse_transmission_color_factor", [1.0,1.0,1.0])
   def dtc_white = abs(float(dtc_live.get(0, 1.0)) - 1.0) < 0.0001 &&
   abs(float(dtc_live.get(1, 1.0)) - 1.0) < 0.0001 &&
   abs(float(dtc_live.get(2, 1.0)) - 1.0) < 0.0001
   if !dtc_white { return spec }
   mut out = spec
   def ssc_live = spec.get("subsurface_color_factor", [1.0,1.0,1.0])
   def ssc_white = abs(float(ssc_live.get(0, 1.0)) - 1.0) < 0.0001 &&
   abs(float(ssc_live.get(1, 1.0)) - 1.0) < 0.0001 &&
   abs(float(ssc_live.get(2, 1.0)) - 1.0) < 0.0001
   if ssc_white {
      def att_live = spec.get("attenuation_color", [1.0,1.0,1.0])
      def ar = float(att_live.get(0, 1.0))
      def ag = float(att_live.get(1, 1.0))
      def ab = float(att_live.get(2, 1.0))
      out["diffuse_transmission_color_factor"] = [
         clamp01(ar * 0.46),
         clamp01(max(ag * 1.08, ab * 1.05)),
         clamp01(max(ab * 1.48, ag * 1.18))
      ]
   } else {
      out["diffuse_transmission_color_factor"] = ssc_live
   }
   out
}

fn _mat_resolve_ext2_slot(
   dict spec, list mr_uv_xf,
   int met_rough_id, int met_rough_uv_set,
   int transmission_id, int sheen_roughness_id, int iridescence_thickness_id, int anisotropy_id,
   int clearcoat_roughness_id, int diffuse_transmission_id, int diffuse_transmission_color_id,
   int emissive_id, int emissive_texcoord, list emit_uv_xf,
   list transmission_uv_xf, list sheen_rough_uv_xf, list iri_thick_uv_xf, list anisotropy_uv_xf,
   list clearcoat_rough_uv_xf, list diffuse_transmission_uv_xf, list diffuse_transmission_color_uv_xf
) list {
   mut ext2_kind = 0
   mut ext2_id = -1
   mut ext2_uv_set = 0
   mut mr_xf = mr_uv_xf
   mut emit_id = int(emissive_id)
   mut emit_tc = int(emissive_texcoord)
   mut emit_xf = emit_uv_xf
   mut emit_is_transmission = false
   mut emit_is_diffuse_transmission = false
   def mr_has_real = int(met_rough_id) >= 0
   def ext2_t0 = _mat_ext2_try_tex(ext2_kind, ext2_id, ext2_uv_set, mr_xf, transmission_id, 1, spec, "transmission_texcoord", transmission_uv_xf, mr_has_real, met_rough_uv_set)
   ext2_kind, ext2_id, ext2_uv_set, mr_xf = int(ext2_t0.get(0, ext2_kind)), int(ext2_t0.get(1, ext2_id)), int(ext2_t0.get(2, ext2_uv_set)), ext2_t0.get(3, mr_xf)
   def ext2_t1 = _mat_ext2_try_tex(ext2_kind, ext2_id, ext2_uv_set, mr_xf, sheen_roughness_id, 2, spec, "sheen_roughness_texcoord", sheen_rough_uv_xf, mr_has_real, met_rough_uv_set)
   ext2_kind, ext2_id, ext2_uv_set, mr_xf = int(ext2_t1.get(0, ext2_kind)), int(ext2_t1.get(1, ext2_id)), int(ext2_t1.get(2, ext2_uv_set)), ext2_t1.get(3, mr_xf)
   def ext2_t2 = _mat_ext2_try_tex(ext2_kind, ext2_id, ext2_uv_set, mr_xf, iridescence_thickness_id, 3, spec, "iridescence_thickness_texcoord", iri_thick_uv_xf, mr_has_real, met_rough_uv_set)
   ext2_kind, ext2_id, ext2_uv_set, mr_xf = int(ext2_t2.get(0, ext2_kind)), int(ext2_t2.get(1, ext2_id)), int(ext2_t2.get(2, ext2_uv_set)), ext2_t2.get(3, mr_xf)
   def ext2_t3 = _mat_ext2_try_tex(ext2_kind, ext2_id, ext2_uv_set, mr_xf, anisotropy_id, 4, spec, "anisotropy_texcoord", anisotropy_uv_xf, mr_has_real, met_rough_uv_set)
   ext2_kind, ext2_id, ext2_uv_set, mr_xf = int(ext2_t3.get(0, ext2_kind)), int(ext2_t3.get(1, ext2_id)), int(ext2_t3.get(2, ext2_uv_set)), ext2_t3.get(3, mr_xf)
   def ext2_aniso = _mat_ext2_try_anisotropy_factor(ext2_kind, ext2_id, ext2_uv_set, spec)
   ext2_kind, ext2_id, ext2_uv_set = int(ext2_aniso.get(0, ext2_kind)), int(ext2_aniso.get(1, ext2_id)), int(ext2_aniso.get(2, ext2_uv_set))
   def ext2_t4 = _mat_ext2_try_tex(ext2_kind, ext2_id, ext2_uv_set, mr_xf, clearcoat_roughness_id, 5, spec, "clearcoat_roughness_texcoord", clearcoat_rough_uv_xf, mr_has_real, met_rough_uv_set)
   ext2_kind, ext2_id, ext2_uv_set, mr_xf = int(ext2_t4.get(0, ext2_kind)), int(ext2_t4.get(1, ext2_id)), int(ext2_t4.get(2, ext2_uv_set)), ext2_t4.get(3, mr_xf)
   def ext2_t5 = _mat_ext2_try_diffuse_transmission(ext2_kind, ext2_id, ext2_uv_set, mr_xf, diffuse_transmission_id, diffuse_transmission_color_id, spec, diffuse_transmission_uv_xf, diffuse_transmission_color_uv_xf, mr_has_real, met_rough_uv_set)
   ext2_kind, ext2_id, ext2_uv_set, mr_xf = int(ext2_t5.get(0, ext2_kind)), int(ext2_t5.get(1, ext2_id)), int(ext2_t5.get(2, ext2_uv_set)), ext2_t5.get(3, mr_xf)
   if ext2_id < 0 && emit_id < 0 && diffuse_transmission_id >= 0 {
      emit_id = diffuse_transmission_id
      emit_tc = int(spec.get("diffuse_transmission_texcoord", 0))
      emit_xf = diffuse_transmission_uv_xf
      emit_is_diffuse_transmission = true
   }
   if ext2_id < 0 && emit_id < 0 && transmission_id >= 0 {
      emit_id = transmission_id
      emit_tc = int(spec.get("transmission_texcoord", 0))
      emit_xf = transmission_uv_xf
      emit_is_transmission = true
   }
   [
      mr_xf,
      emit_id,
      emit_tc,
      emit_xf,
      emit_is_transmission,
      emit_is_diffuse_transmission,
      _mat_pack_ext2_tex_word(ext2_kind, ext2_id, ext2_uv_set, spec)
   ]
}

fn _mat_vec3_is(any minfo, str key, f64 x, f64 y, f64 z) bool {
   def v = minfo.get(key, [x, y, z])
   abs(float(v.get(0, x)) - float(x)) <= 0.000001 &&
   abs(float(v.get(1, y)) - float(y)) <= 0.000001 &&
   abs(float(v.get(2, z)) - float(z)) <= 0.000001
}

fn _mat_float_is(any minfo, str key, f64 default_v) bool {
   abs(float(minfo.get(key, default_v)) - float(default_v)) <= 0.000001
}

fn _mat_float_keys_zero(any minfo, any keys) bool {
   mut i = 0
   def n = keys.len
   while i < n {
      if !_mat_float_is(minfo, keys.get(i, ""), 0.0) { return false }
      i += 1
   }
   true
}

fn _mat_info_has_extra_texture_slots(any minfo) bool {
   mut ti = 1
   def tex_slots_n = _MAT_SPEC_TEX_SLOTS.len
   while ti < tex_slots_n {
      def row = _MAT_SPEC_TEX_SLOTS.get(ti, 0)
      def prefix = to_str(row.get(0, ""))
      if prefix.len > 0 && to_str(minfo.get(prefix + "_uri", "")).len > 0 { return true }
      ti += 1
   }
   false
}

fn _mat_info_fast_base_ok(any minfo) bool {
   if bool(minfo.get("specular_glossiness", false)) { return false }
   if bool(minfo.get("unlit", false)) { return false }
   if str.upper(to_str(minfo.get("alpha_mode", "OPAQUE"))) != "OPAQUE" { return false }
   if _mat_info_has_extra_texture_slots(minfo) { return false }
   def ef = minfo.get("emissive_factor", [0.0, 0.0, 0.0])
   if abs(float(ef.get(0, 0.0))) > 0.000001
   || abs(float(ef.get(1, 0.0))) > 0.000001
   || abs(float(ef.get(2, 0.0))) > 0.000001{
      return false
   }
   if abs(float(minfo.get("emissive_strength", 1.0)) - 1.0) > 0.000001 { return false }
   true
}

fn _mat_info_fast_surface_ext_ok(any minfo) bool {
   if !_mat_float_keys_zero(minfo, _MAT_FAST_SURFACE_ZERO_KEYS) { return false }
   if !_mat_vec3_is(minfo, "sheen_color_factor", 0.0, 0.0, 0.0) { return false }
   true
}

fn _mat_info_fast_volume_ext_ok(any minfo) bool {
   if !_mat_float_keys_zero(minfo, _MAT_FAST_VOLUME_ZERO_KEYS) { return false }
   if !_mat_float_is(minfo, "ior", 1.5) { return false }
   if !_mat_float_is(minfo, "specular_factor", 1.0) { return false }
   if !_mat_vec3_is(minfo, "specular_color_factor", 1.0, 1.0, 1.0) { return false }
   if !_mat_vec3_is(minfo, "attenuation_color", 1.0, 1.0, 1.0) { return false }
   if !_mat_vec3_is(minfo, "diffuse_transmission_color_factor", 1.0, 1.0, 1.0) { return false }
   if !_mat_vec3_is(minfo, "subsurface_color_factor", 1.0, 1.0, 1.0) { return false }
   if !_mat_float_is(minfo, "alpha_coverage", 1.0) { return false }
   true
}

fn _mat_info_fast_core_pbr_ok(any minfo) bool {
   if !is_dict(minfo) { return false }
   if !_mat_info_fast_base_ok(minfo) { return false }
   if !_mat_info_fast_surface_ext_ok(minfo) { return false }
   _mat_info_fast_volume_ext_ok(minfo)
}

fn _mat_infos_fast_core_pbr_ok(any material_infos) bool {
   if !is_list(material_infos) { return false }
   mut i = 0
   def n = material_infos.len
   while i < n {
      if !_mat_info_fast_core_pbr_ok(material_infos.get(i, 0)) { return false }
      i += 1
   }
   n > 0
}

fn _mat_info_req_load_base(dict req_map, any minfo, str base_path) dict {
   def uri = to_str(minfo.get("base_color_uri", ""))
   if uri.len == 0 { return req_map }
   def filter = int(minfo.get("base_color_filter", -1))
   def sampler = minfo.get("base_color_sampler", 0)
   def key = _mat_tex_req_key(base_path, "base_color", uri, filter, sampler)
   return _mat_tex_req_load(req_map, key, uri, base_path, "base_color", filter, sampler)
}

fn _mat_info_load_base_tex(any tex_req_map, bool predecode_active, any minfo, str base_path) int {
   def uri = to_str(minfo.get("base_color_uri", ""))
   if uri.len == 0 { return -1 }
   def filter = int(minfo.get("base_color_filter", -1))
   def sampler = minfo.get("base_color_sampler", 0)
   if predecode_active { return _stable_loaded_texture_id(_mat_tex_req_get(tex_req_map, _mat_tex_req_key(base_path, "base_color", uri, filter, sampler))) }
   return _stable_loaded_texture_id(_try_load_tex(uri, base_path, "base_color", filter, sampler))
}

fn _mat_pack_base_color_u32(any base_color_factor) int {
   def fr = band(int((clamp01(float(base_color_factor.get(0, 1.0))) * 255.0)), 255)
   def fg = band(int((clamp01(float(base_color_factor.get(1, 1.0))) * 255.0)), 255)
   def fb = band(int((clamp01(float(base_color_factor.get(2, 1.0))) * 255.0)), 255)
   def fa = band(int((clamp01(float(base_color_factor.get(3, 1.0))) * 255.0)), 255)
   bor(bor(fr, bshl(fg, 8)), bor(bshl(fb, 16), bshl(fa, 24)))
}

fn _build_material_records_fast_core_pbr(list material_infos, str base_path, bool diag_on, bool stage_trace) list {
   def mat_count = material_infos.len
   mut tex_req_map = dict(256)
   def predecode_active = _gltf_predecode_batch_enabled(mat_count)
   _scene_stage(stage_trace, "materials.predecode active=" + to_str(predecode_active) + " fast=core_pbr")
   if predecode_active {
      def t_pre0 = ticks()
      mut pre_i = 0
      while pre_i < mat_count {
         def minfo = material_infos.get(pre_i, 0)
         if is_dict(minfo) { tex_req_map = _mat_info_req_load_base(tex_req_map, minfo, base_path) }
         pre_i += 1
      }
      _scene_stage(stage_trace, "materials.predecode.queued keys=" + to_str(len(keys(tex_req_map))) + " ms=" + to_str(ui_profile.elapsed_ms(t_pre0)))
      tex_req_map = _mat_tex_req_realize(tex_req_map)
   }
   mut mat_records = list(0)
   mut loaded_tex_count = 0
   def normal_tex_word_default = render_utils.pack_normal_tex_word(-1, 0, 1.0, false)
   mut mi = 0
   while mi < mat_count {
      def minfo = material_infos.get(mi, 0)
      def base_id = _mat_info_load_base_tex(tex_req_map, predecode_active, minfo, base_path)
      if base_id >= 0 { loaded_tex_count += 1 }
      def base_color_u32 = _mat_pack_base_color_u32(minfo.get("base_color_factor", [1.0, 1.0, 1.0, 1.0]))
      def metallic_u8 = band(int((clamp01(float(minfo.get("metallic_factor", 1.0))) * 255.0)), 255)
      def rough_u8 = band(int((clamp01(float(minfo.get("roughness_factor", 1.0))) * 255.0)), 255)
      def material_u32 = bor(band(metallic_u8, 255), bshl(band(rough_u8, 255), 8))
      def alpha_cutoff_u8 = band(int((clamp01(float(minfo.get("alpha_cutoff", 0.5))) * 255.0)), 255)
      def occlusion_strength_u8 = band(int((clamp01(float(minfo.get("occlusion_strength", 1.0))) * 255.0)), 255)
      def alpha_u32 = bor(bshl(alpha_cutoff_u8, 8), bshl(occlusion_strength_u8, 16))
      mat_records = mat_records.append({
            "base_color_u32": base_color_u32, "base": base_id,
            "normal": -1, "normal_tex_word": normal_tex_word_default, "normal_uv_set": 0,
            "metallic_roughness": -1, "metallic_roughness_uv_set": 0,
            "occlusion": -1, "occlusion_uv_set": 0,
            "emissive": -1, "emissive_factor": [0.0, 0.0, 0.0], "emissive_strength": 1.0,
            "emissive_u32": 0, "emissive_uv_set": 0,
            "alpha_u32": alpha_u32, "material_u32": material_u32,
            "base_uv_xf0": int(minfo.get("base_color_uv_xf0", 0)), "base_uv_xf1": int(minfo.get("base_color_uv_xf1", 0)),
            "normal_uv_xf0": 0, "normal_uv_xf1": 0,
            "mr_uv_xf0": 0, "mr_uv_xf1": 0,
            "occlusion_uv_xf0": 0, "occlusion_uv_xf1": 0,
            "emissive_uv_xf0": 0, "emissive_uv_xf1": 0,
            "bsdf0_u32": 0x000000ff, "bsdf1_u32": 0x55ffffff, "bsdf2_u32": 0, "bsdf3_u32": 0xffffffff,
            "bsdf4_u32": 0, "bsdf5_u32": 0xff000000, "bsdf_ext_slab": 0,
            "ext2_tex_word": 0x80000000, "vc_mode": 0, "fast_core_pbr": true,
            "double_sided": minfo.get("double_sided", false) ? true : false,
            "base_color_texcoord": int(minfo.get("base_color_texcoord", 0))
      })
      mi += 1
   }
   if diag_on { terminal.log("[gltf] Loaded " + to_str(loaded_tex_count) + " textures for " + to_str(mat_count) + " materials") }
   _scene_stage(stage_trace, "materials.fast_core_pbr count=" + to_str(mat_records.len))
   return mat_records
}

fn _mat_predecode_request_map(any mat_specs, str base_path, int mat_count, bool stage_trace) dict {
   mut tex_req_map = dict(256)
   def predecode_active = _gltf_predecode_batch_enabled(mat_count)
   _scene_stage(stage_trace, "materials.predecode active=" + to_str(predecode_active))
   if predecode_active {
      def t_pre0 = ticks()
      mut pre_i = 0
      while pre_i < mat_count {
         mut spec = mat_specs.get(pre_i, 0)
         if is_dict(spec) {
            spec = _bake_ext_texture_factors(spec, base_path)
            tex_req_map = _mat_tex_req_load_slots(tex_req_map, spec, base_path)
         }
         pre_i += 1
      }
      _scene_stage(stage_trace, "materials.predecode.queued keys=" + to_str(len(keys(tex_req_map))) + " ms=" + to_str(ui_profile.elapsed_ms(t_pre0)))
      tex_req_map = _mat_tex_req_realize(tex_req_map)
   }
   {"active": predecode_active, "req_map": tex_req_map}
}

fn _mat_emissive_uv_flags(bool has_minfo, int emissive_texcoord, bool emissive_is_transmission, bool emissive_is_diffuse_transmission) int {
   mut emissive_uv_set = 0
   if has_minfo && emissive_texcoord == 1 { emissive_uv_set = 1 }
   if has_minfo && emissive_is_transmission { emissive_uv_set = emissive_uv_set | 2 }
   if has_minfo && emissive_is_diffuse_transmission { emissive_uv_set = emissive_uv_set | 4 }
   emissive_uv_set
}

fn _mat_mr_word(int met_rough_id, int met_rough_uv_set) int {
   mut mr_word = 0
   if met_rough_id >= 0 { mr_word = band(met_rough_id + 1, 0x7fff) }
   if met_rough_uv_set == 1 { mr_word = bor(mr_word, 0x8000) }
   mr_word
}

fn _mat_alpha_mode_code(str alpha_mode) int {
   return case alpha_mode {
      "MASK" -> 1
      "BLEND" -> 2
      _ -> 0
   }
}

fn _mat_clamp01_vec3(any v) list {
   [
      clamp01(float(v.get(0, 1.0))),
      clamp01(float(v.get(1, 1.0))),
      clamp01(float(v.get(2, 1.0)))
   ]
}

fn _mat_diffuse_transmission_color_slot(dict spec, int bsdf2_u32, int sheen_color_id) list {
   def dt_factor = float(spec.get("diffuse_transmission_factor", 0.0))
   def sheen_color_factor = spec.get("sheen_color_factor", [0.0, 0.0, 0.0])
   def sheen_color_energy =
   float(sheen_color_factor.get(0, 0.0)) +
   float(sheen_color_factor.get(1, 0.0)) +
   float(sheen_color_factor.get(2, 0.0))
   if dt_factor <= 0.000001 || sheen_color_energy > 0.000001 || sheen_color_id >= 0 {
      return [bsdf2_u32, false]
   }
   def dtc = spec.get("diffuse_transmission_color_factor", [1.0, 1.0, 1.0])
   def dt_r = band(int((clamp01(float(dtc.get(0, 1.0))) * 255.0)), 255)
   def dt_g = band(int((clamp01(float(dtc.get(1, 1.0))) * 255.0)), 255)
   def dt_b = band(int((clamp01(float(dtc.get(2, 1.0))) * 255.0)), 255)
   def thick_u8 = band(bshr(bsdf2_u32, 24), 255)
   [bor(dt_r, bor(bshl(dt_g, 8), bor(bshl(dt_b, 16), bshl(thick_u8, 24)))), true]
}

fn _mat_try_aux_tex(dict spec, int current_id, int current_uv_set, list current_uv_xf, int alpha_u32, int candidate_id, str texcoord_key, list candidate_uv_xf, int flag) list {
   if current_id >= 0 || candidate_id < 0 { return [current_id, current_uv_set, current_uv_xf, alpha_u32] }
   [candidate_id, _scene_texcoord_one(spec, texcoord_key), candidate_uv_xf, bor(alpha_u32, flag)]
}

fn _mat_try_emissive_aux_tex(dict spec, int current_id, int current_texcoord, list current_uv_xf, int alpha_u32, int candidate_id, str texcoord_key, list candidate_uv_xf, int flag) list {
   if current_id >= 0 || candidate_id < 0 { return [current_id, current_texcoord, current_uv_xf, alpha_u32] }
   [candidate_id, int(spec.get(texcoord_key, 0)), candidate_uv_xf, bor(alpha_u32, flag)]
}

fn _mat_aux_state_get(any state, int idx, any fallback) any {
   is_list(state) ? state.get(idx, fallback) : fallback
}

fn _mat_apply_aux_slot(dict spec, any out, any state, any row, bool emissive=false) list {
   def cur_id = int(_mat_aux_state_get(state, 0, -1))
   def cur_uv = int(_mat_aux_state_get(state, 1, 0))
   def cur_xf = _mat_aux_state_get(state, 2, [0, 0])
   def alpha = int(_mat_aux_state_get(state, 3, 0))
   def candidate_id = int(out.get(to_str(row.get(0, "")), -1))
   def texcoord_key = to_str(row.get(1, ""))
   def candidate_uv_xf = out.get(to_str(row.get(2, "")), [0, 0])
   def flag = int(row.get(3, 0))
   if emissive {
      return _mat_try_emissive_aux_tex(spec, cur_id, cur_uv, cur_xf, alpha, candidate_id, texcoord_key, candidate_uv_xf, flag)
   }
   _mat_try_aux_tex(spec, cur_id, cur_uv, cur_xf, alpha, candidate_id, texcoord_key, candidate_uv_xf, flag)
}

fn _mat_apply_aux_slots(dict spec, any out, any state, any slots, bool emissive=false) list {
   mut aux_state = state
   mut i = 0
   def slots_n = slots.len
   while i < slots_n {
      aux_state = _mat_apply_aux_slot(spec, out, aux_state, slots.get(i, []), emissive)
      i += 1
   }
   aux_state
}

fn _mat_log_request_summary(int mi, any spec, str base_path, str base_uri, str normal_uri, str mr_uri, str occ_uri, int base_id, int normal_id, int met_rough_id, int occlusion_id) bool {
   if !_gltf_mat_summary_enabled() { return false }
   def base_key = _mat_tex_slot_key(spec, base_path, "base_color", "color")
   def normal_key = _mat_tex_slot_key(spec, base_path, "normal", "normal")
   def mr_key = _mat_tex_slot_key(spec, base_path, "metallic_roughness", "metallic_roughness")
   def occ_key = _mat_tex_slot_key(spec, base_path, "occlusion", "occlusion")
   ui_profile.print_text("[gltf:matreq] idx=" + to_str(mi) +
      " base_uri=" + base_uri +
      " normal_uri=" + normal_uri +
      " mr_uri=" + mr_uri +
      " occ_uri=" + occ_uri +
      " base_key=" + base_key +
      " normal_key=" + normal_key +
      " mr_key=" + mr_key +
      " occ_key=" + occ_key +
      " base_id=" + to_str(base_id) +
      " normal_id=" + to_str(normal_id) +
      " mr_id=" + to_str(met_rough_id) +
   " occ_id=" + to_str(occlusion_id))
   true
}

fn _mat_log_pack_summary(int mi, any spec, bool has_minfo, int base_id, int met_rough_id, int emissive_id, int ext2_tex_word, int bsdf0_u32, int bsdf2_u32, int bsdf4_u32, int bsdf5_u32) bool {
   if !_gltf_mat_summary_enabled() { return false }
   def live_trans = has_minfo ? float(spec.get("transmission_factor", 0.0)) : 0.0
   def live_thick = has_minfo ? float(spec.get("thickness_factor", 0.0)) : 0.0
   def live_disp = has_minfo ? float(spec.get("dispersion", 0.0)) : 0.0
   ui_profile.print_text("[gltf:matpack] idx=" + to_str(mi) +
      " base=" + to_str(base_id) +
      " mr=" + to_str(met_rough_id) +
      " emit=" + to_str(emissive_id) +
      " ext2=0x" + str.to_hex(ext2_tex_word) +
      " bsdf0=0x" + str.to_hex(bsdf0_u32) +
      " bsdf2=0x" + str.to_hex(bsdf2_u32) +
      " bsdf4=0x" + str.to_hex(bsdf4_u32) +
      " bsdf5=0x" + str.to_hex(bsdf5_u32) +
      " trans=" + to_str(live_trans) +
      " thick=" + to_str(live_thick) +
   " disp=" + to_str(live_disp))
   true
}

fn _mat_log_diag(int mi, int base_id, int normal_id, int met_rough_id, int occlusion_id, int emissive_id, int base_color_u32, int alpha_u32, int mat_word, bool diag_on) bool {
   if !diag_on { return false }
   terminal.log(
      "[mat " + to_str(mi)
      + "] base=" + to_str(base_id)
      + " nrm=" + to_str(normal_id)
      + " mr=" + to_str(met_rough_id)
      + " occ=" + to_str(occlusion_id)
      + " emit=" + to_str(emissive_id)
      + " tint=0x" + str.to_hex(base_color_u32)
      + " alpha=0x" + str.to_hex(alpha_u32)
      + " mat=0x" + str.to_hex(mat_word)
   )
   true
}

fn _mat_slow_default_state(any spec) dict {
   mut st = dict(96)
   st["spec"] = spec
   st["has_minfo"] = is_dict(spec)
   st["metallic_u8"] = 255
   st["rough_u8"] = 255
   st["base_id"] = -1
   st["normal_id"] = -1
   st["met_rough_id"] = -1
   st["occlusion_id"] = -1
   st["emissive_id"] = -1
   st["specular_id"] = -1
   st["specular_color_id"] = -1
   st["thickness_id"] = -1
   st["iridescence_id"] = -1
   st["clearcoat_id"] = -1
   st["clearcoat_normal_id"] = -1
   st["clearcoat_roughness_id"] = -1
   st["sheen_color_id"] = -1
   st["sheen_roughness_id"] = -1
   st["transmission_id"] = -1
   st["iridescence_thickness_id"] = -1
   st["anisotropy_id"] = -1
   st["diffuse_transmission_id"] = -1
   st["diffuse_transmission_color_id"] = -1
   st["base_color_u32"] = 0xffffffff
   st["alpha_u32"] = 0
   st["normal_uv_set"] = 0
   st["occlusion_uv_set"] = 0
   st["met_rough_uv_set"] = 0
   st["active_normal_scale"] = 1.0
   st["clearcoat_normal_only"] = false
   st["emissive_is_transmission"] = false
   st["emissive_is_diffuse_transmission"] = false
   st["emissive_texcoord"] = 0
   st["base_uv_xf"] = [0, 0]
   st["normal_uv_xf"] = [0, 0]
   st["mr_uv_xf"] = [0, 0]
   st["occ_uv_xf"] = [0, 0]
   st["emit_uv_xf"] = [0, 0]
   st["bsdf0_u32"] = 0
   st["bsdf1_u32"] = 0
   st["bsdf2_u32"] = 0
   st["bsdf3_u32"] = 0
   st["loaded_count"] = 0
   st["transmission_factor_live"] = 0.0
   st["use_dt_color_factor_slot"] = false
   st
}

fn _mat_slow_factor_state(dict st) dict {
   mut out = st
   mut spec = out.get("spec", dict(0))
   def base_color_factor = spec.get("base_color_factor", [1.0, 1.0, 1.0, 1.0])
   out["base_color_u32"] = _mat_pack_base_color_u32(base_color_factor)
   mut metallic_u8 = band(int((clamp01(float(spec.get("metallic_factor", 1.0))) * 255.0)), 255)
   if spec.get("specular_glossiness", false) { metallic_u8 = 0 }
   out["metallic_u8"] = metallic_u8
   out["rough_u8"] = band(int((clamp01(float(spec.get("roughness_factor", 1.0))) * 255.0)), 255)
   out["emissive_texcoord"] = int(spec.get("emissive_texcoord", 0))
   out["normal_uv_set"] = _scene_texcoord_one(spec, "normal_texcoord")
   out["occlusion_uv_set"] = _scene_texcoord_one(spec, "occlusion_texcoord")
   out["met_rough_uv_set"] = _scene_texcoord_one(spec, "metallic_roughness_texcoord")
   out["active_normal_scale"] = float(spec.get("normal_scale", 1.0))
   def transmission_factor_live = float(spec.get("transmission_factor", 0.0))
   def diffuse_transmission_factor_live = float(spec.get("diffuse_transmission_factor", 0.0))
   out["transmission_factor_live"] = transmission_factor_live
   spec = _mat_apply_diffuse_transmission_color_fallback(spec, diffuse_transmission_factor_live)
   def alpha_mode_code = _mat_alpha_mode_code(str.upper(to_str(spec.get("alpha_mode", "OPAQUE"))))
   def alpha_cutoff_u8 = band(int((clamp01(float(spec.get("alpha_cutoff", 0.5))) * 255.0)), 255)
   def occlusion_strength_u8 = band(int((clamp01(float(spec.get("occlusion_strength", 1.0))) * 255.0)), 255)
   mut alpha_u32 = bor(alpha_mode_code, bor(bshl(alpha_cutoff_u8, 8), bshl(occlusion_strength_u8, 16)))
   def spec_col_factor = spec.get("specular_color_factor", [1.0, 1.0, 1.0])
   def spec_col_peak = max(
      float(spec_col_factor.get(0, 1.0)),
      max(float(spec_col_factor.get(1, 1.0)), float(spec_col_factor.get(2, 1.0)))
   )
   def has_volume_like = transmission_factor_live > 0.000001
   || float(spec.get("thickness_factor", 0.0)) > 0.000001
   || diffuse_transmission_factor_live > 0.000001
   || float(spec.get("refraction_factor", 0.0)) > 0.000001
   || float(spec.get("subsurface_factor", 0.0)) > 0.000001
   if spec_col_peak > 1.001 && !has_volume_like { spec["specular_color_factor"] = _mat_clamp01_vec3(spec_col_factor) }
   out["spec"] = spec
   out["alpha_u32"] = alpha_u32
   out
}

fn _mat_slow_texture_state(dict st, any tex_req_map, bool predecode_active, str base_path, int mi) dict {
   mut out = st
   def spec = out.get("spec", dict(0))
   def tex_ids = _mat_tex_resolved_slots(tex_req_map, predecode_active, spec, base_path)
   mut base_id = int(tex_ids.get("base_color", -1))
   ;; Defensive fallback: the batch/predecode path is an optimization.  If it
   ;; fails to materialize the base-color texture, retry the exact material URI
   ;; directly before we build GPU records.  Otherwise textured glTFs can fall
   ;; back to a gray baseColorFactor and look like the Avocado gray-stripe bug.
   if base_id < 0 && to_str(spec.get("base_color_uri", "")).len > 0 {
      base_id = _try_load_tex(
         spec.get("base_color_uri", ""),
         base_path,
         "base_color",
         int(spec.get("base_color_filter", -1)),
         spec.get("base_color_sampler", 0)
      )
   }
   out["base_id"] = base_id
   out["normal_id"] = int(tex_ids.get("normal", -1))
   out["met_rough_id"] = int(tex_ids.get("metallic_roughness", -1))
   out["occlusion_id"] = int(tex_ids.get("occlusion", -1))
   out["emissive_id"] = int(tex_ids.get("emissive", -1))
   out["specular_id"] = int(tex_ids.get("specular", -1))
   out["specular_color_id"] = int(tex_ids.get("specular_color", -1))
   out["thickness_id"] = int(tex_ids.get("thickness", -1))
   out["iridescence_id"] = int(tex_ids.get("iridescence", -1))
   out["clearcoat_id"] = int(tex_ids.get("clearcoat", -1))
   out["clearcoat_normal_id"] = int(tex_ids.get("clearcoat_normal", -1))
   out["clearcoat_roughness_id"] = int(tex_ids.get("clearcoat_roughness", -1))
   out["sheen_color_id"] = int(tex_ids.get("sheen_color", -1))
   out["sheen_roughness_id"] = int(tex_ids.get("sheen_roughness", -1))
   out["transmission_id"] = int(tex_ids.get("transmission", -1))
   out["iridescence_thickness_id"] = int(tex_ids.get("iridescence_thickness", -1))
   out["anisotropy_id"] = int(tex_ids.get("anisotropy", -1))
   out["diffuse_transmission_id"] = int(tex_ids.get("diffuse_transmission", -1))
   out["diffuse_transmission_color_id"] = int(tex_ids.get("diffuse_transmission_color", -1))
   out["loaded_count"] = int(tex_ids.get("_loaded_count", 0))
   _mat_log_request_summary(
      mi,
      spec,
      base_path,
      to_str(spec.get("base_color_uri", "")),
      to_str(spec.get("normal_uri", "")),
      to_str(spec.get("metallic_roughness_uri", "")),
      to_str(spec.get("occlusion_uri", "")),
      int(out.get("base_id", -1)),
      int(out.get("normal_id", -1)),
      int(out.get("met_rough_id", -1)),
   int(out.get("occlusion_id", -1)))
   out
}

fn _mat_slow_uv_state(dict st) dict {
   mut out = st
   def spec = out.get("spec", dict(0))
   mut i = 0
   while i < _MAT_SLOW_UV_STATE_FIELDS.len {
      def f = _MAT_SLOW_UV_STATE_FIELDS.get(i)
      out[to_str(f.get(0))] = gltf._gltf_pack_uv_xform_words(spec, to_str(f.get(1)))
      i += 1
   }
   out
}

fn _mat_slow_bsdf_state(dict st) dict {
   mut out = st
   def spec = out.get("spec", dict(0))
   out["bsdf0_u32"] = render_utils.pack_bsdf0_u32(spec)
   out["bsdf1_u32"] = render_utils.pack_bsdf1_u32(spec)
   mut bsdf2_u32 = render_utils.pack_bsdf2_u32(spec)
   out["bsdf3_u32"] = render_utils.pack_bsdf3_u32(spec)
   def dt_color_slot = _mat_diffuse_transmission_color_slot(spec, bsdf2_u32, int(out.get("sheen_color_id", -1)))
   bsdf2_u32 = int(dt_color_slot.get(0, bsdf2_u32))
   out["bsdf2_u32"] = bsdf2_u32
   out["use_dt_color_factor_slot"] = bool(dt_color_slot.get(1, false))
   out
}

fn _mat_slow_apply_occlusion_aux(dict st) dict {
   mut out = st
   def spec = out.get("spec", dict(0))
   mut occlusion_id = int(out.get("occlusion_id", -1))
   mut occlusion_uv_set = int(out.get("occlusion_uv_set", 0))
   mut occ_uv_xf = out.get("occ_uv_xf", [0, 0])
   mut alpha_u32 = int(out.get("alpha_u32", 0))
   def iri_priority_aux = int(out.get("iridescence_id", -1)) >= 0 &&
   float(spec.get("iridescence_factor", 0.0)) > 0.000001 &&
   (float(out.get("transmission_factor_live", 0.0)) > 0.000001 || float(spec.get("thickness_factor", 0.0)) > 0.000001 || occlusion_id < 0)
   if iri_priority_aux {
      occlusion_id = int(out.get("iridescence_id", -1))
      occlusion_uv_set = _scene_texcoord_one(spec, "iridescence_texcoord")
      occ_uv_xf = out.get("iri_uv_xf", [0, 0])
      alpha_u32 = bor(alpha_u32, 0x08000000)
   } else {
      def aux = _mat_apply_aux_slots(spec, out, [occlusion_id, occlusion_uv_set, occ_uv_xf, alpha_u32], _MAT_OCCLUSION_AUX_SLOTS)
      occlusion_id, occlusion_uv_set, occ_uv_xf, alpha_u32 = int(aux.get(0, occlusion_id)), int(aux.get(1, occlusion_uv_set)), aux.get(2, occ_uv_xf), int(aux.get(3, alpha_u32))
   }
   out["occlusion_id"] = occlusion_id
   out["occlusion_uv_set"] = occlusion_uv_set
   out["occ_uv_xf"] = occ_uv_xf
   out["alpha_u32"] = alpha_u32
   out
}

fn _mat_slow_apply_emissive_aux(dict st) dict {
   mut out = st
   def spec = out.get("spec", dict(0))
   mut emissive_id = int(out.get("emissive_id", -1))
   mut emissive_texcoord = int(out.get("emissive_texcoord", 0))
   mut emit_uv_xf = out.get("emit_uv_xf", [0, 0])
   mut alpha_u32 = int(out.get("alpha_u32", 0))
   def em = _mat_apply_aux_slots(spec, out, [emissive_id, emissive_texcoord, emit_uv_xf, alpha_u32], _MAT_EMISSIVE_AUX_SLOTS, true)
   emissive_id, emissive_texcoord, emit_uv_xf, alpha_u32 = int(em.get(0, emissive_id)), int(em.get(1, emissive_texcoord)), em.get(2, emit_uv_xf), int(em.get(3, alpha_u32))
   if bool(out.get("use_dt_color_factor_slot", false)) { alpha_u32 = bor(alpha_u32, 0x80000000) }
   out["emissive_id"] = emissive_id
   out["emissive_texcoord"] = emissive_texcoord
   out["emit_uv_xf"] = emit_uv_xf
   out["alpha_u32"] = alpha_u32
   out
}

fn _mat_slow_apply_clearcoat_normal(dict st) dict {
   mut out = st
   if int(out.get("normal_id", -1)) >= 0 || int(out.get("clearcoat_normal_id", -1)) < 0 { return out }
   def spec = out.get("spec", dict(0))
   out["normal_id"] = int(out.get("clearcoat_normal_id", -1))
   out["normal_uv_set"] = _scene_texcoord_one(spec, "clearcoat_normal_texcoord")
   out["normal_uv_xf"] = out.get("clearcoat_normal_uv_xf", [0, 0])
   out["active_normal_scale"] = float(spec.get("clearcoat_normal_scale", 1.0))
   out["clearcoat_normal_only"] = true
   out
}

fn _mat_slow_apply_ext2(dict st) dict {
   mut out = st
   mut spec = out.get("spec", dict(0))
   def ext2_state = _mat_resolve_ext2_slot(
      spec,
      out.get("mr_uv_xf", [0, 0]),
      int(out.get("met_rough_id", -1)),
      int(out.get("met_rough_uv_set", 0)),
      int(out.get("transmission_id", -1)),
      int(out.get("sheen_roughness_id", -1)),
      int(out.get("iridescence_thickness_id", -1)),
      int(out.get("anisotropy_id", -1)),
      int(out.get("clearcoat_roughness_id", -1)),
      int(out.get("diffuse_transmission_id", -1)),
      int(out.get("diffuse_transmission_color_id", -1)),
      int(out.get("emissive_id", -1)),
      int(out.get("emissive_texcoord", 0)),
      out.get("emit_uv_xf", [0, 0]),
      out.get("transmission_uv_xf", [0, 0]),
      out.get("sheen_rough_uv_xf", [0, 0]),
      out.get("iri_thick_uv_xf", [0, 0]),
      out.get("anisotropy_uv_xf", [0, 0]),
      out.get("clearcoat_rough_uv_xf", [0, 0]),
      out.get("diffuse_transmission_uv_xf", [0, 0]),
   out.get("diffuse_transmission_color_uv_xf", [0, 0]))
   out["mr_uv_xf"] = ext2_state.get(0, out.get("mr_uv_xf", [0, 0]))
   out["emissive_id"] = int(ext2_state.get(1, out.get("emissive_id", -1)))
   out["emissive_texcoord"] = int(ext2_state.get(2, out.get("emissive_texcoord", 0)))
   out["emit_uv_xf"] = ext2_state.get(3, out.get("emit_uv_xf", [0, 0]))
   out["emissive_is_transmission"] = bool(ext2_state.get(4, false))
   out["emissive_is_diffuse_transmission"] = bool(ext2_state.get(5, false))
   spec["ext2_tex_word"] = int(ext2_state.get(6, 0x80000000))
   out["spec"] = spec
   out
}

fn _mat_slow_apply_aux_state(dict st) dict {
   mut out = _mat_slow_apply_occlusion_aux(st)
   out = _mat_slow_apply_emissive_aux(out)
   out = _mat_slow_apply_clearcoat_normal(out)
   _mat_slow_apply_ext2(out)
}

fn _mat_slow_finalize_core_record(dict st, bool has_minfo, dict spec, int normal_tex_word, int emissive_uv_set) dict {
   def emissive_factor = has_minfo ? spec.get("emissive_factor", [0.0, 0.0, 0.0]) : [0.0, 0.0, 0.0]
   def emissive_strength = has_minfo ? float(spec.get("emissive_strength", 1.0)) : 1.0
   {
      "base_color_u32": int(st.get("base_color_u32", 0xffffffff)),
      "base": int(st.get("base_id", -1)),
      "normal": int(st.get("normal_id", -1)),
      "normal_tex_word": normal_tex_word,
      "normal_uv_set": int(st.get("normal_uv_set", 0)),
      "metallic_roughness": int(st.get("met_rough_id", -1)),
      "metallic_roughness_uv_set": int(st.get("met_rough_uv_set", 0)),
      "occlusion": int(st.get("occlusion_id", -1)),
      "occlusion_uv_set": int(st.get("occlusion_uv_set", 0)),
      "emissive": int(st.get("emissive_id", -1)),
      "emissive_factor": emissive_factor,
      "emissive_strength": emissive_strength,
      "emissive_u32": render_utils.pack_emissive_u32(emissive_factor, emissive_strength),
      "emissive_uv_set": emissive_uv_set,
      "alpha_u32": int(st.get("alpha_u32", 0)),
      "material_u32": gltf._pack_material_word(int(st.get("metallic_u8", 255)), int(st.get("rough_u8", 255)), _mat_mr_word(int(st.get("met_rough_id", -1)), int(st.get("met_rough_uv_set", 0))))
   }
}

fn _mat_slow_record_add_uv_fields(dict rec, dict st) dict {
   mut out = rec
   mut i = 0
   while i < _MAT_RECORD_UV_FIELDS.len {
      def f = _MAT_RECORD_UV_FIELDS.get(i)
      def uv_xf = st.get(to_str(f.get(0)), [0, 0])
      out[to_str(f.get(1))] = int(uv_xf.get(0, 0))
      out[to_str(f.get(2))] = int(uv_xf.get(1, 0))
      i += 1
   }
   out
}

fn _mat_slow_record_add_bsdf_fields(dict rec, dict st, bool has_minfo, dict spec) dict {
   mut out = rec
   out["bsdf0_u32"] = int(st.get("bsdf0_u32", 0))
   out["bsdf1_u32"] = int(st.get("bsdf1_u32", 0))
   out["bsdf2_u32"] = int(st.get("bsdf2_u32", 0))
   out["bsdf3_u32"] = int(st.get("bsdf3_u32", 0))
   out["bsdf4_u32"] = has_minfo ? render_utils.pack_bsdf4_u32(spec) : 0
   out["bsdf5_u32"] = has_minfo ? render_utils.pack_bsdf5_u32(spec) : 0
   out["bsdf_ext_slab"] = has_minfo ? render_utils.pack_bsdf_ext_slab(spec) : 0
   out["ext2_tex_word"] = has_minfo ? int(spec.get("ext2_tex_word", 0x80000000)) : 0x80000000
   out["vc_mode"] = has_minfo && spec.get("specular_glossiness", false) ? 2 : 0
   out
}

fn _mat_slow_finalize_record(dict st) dict {
   def has_minfo = bool(st.get("has_minfo", false))
   def spec = has_minfo ? st.get("spec", dict(0)) : dict(0)
   def normal_tex_word = render_utils.pack_normal_tex_word(
      int(st.get("normal_id", -1)),
      int(st.get("normal_uv_set", 0)),
      float(st.get("active_normal_scale", 1.0)),
   bool(st.get("clearcoat_normal_only", false)))
   def emissive_uv_set = _mat_emissive_uv_flags(
      has_minfo,
      int(st.get("emissive_texcoord", 0)),
      bool(st.get("emissive_is_transmission", false)),
   bool(st.get("emissive_is_diffuse_transmission", false)))
   mut rec = _mat_slow_finalize_core_record(st, has_minfo, spec, normal_tex_word, emissive_uv_set)
   rec = _mat_slow_record_add_uv_fields(rec, st)
   _mat_slow_record_add_bsdf_fields(rec, st, has_minfo, spec)
}

fn _mat_slow_record_from_spec(any spec, any tex_req_map, bool predecode_active, str base_path, int mi, bool diag_on) dict {
   mut st = _mat_slow_default_state(spec)
   if bool(st.get("has_minfo", false)) {
      st = _mat_slow_factor_state(st)
      st = _mat_slow_texture_state(st, tex_req_map, predecode_active, base_path, mi)
      st = _mat_slow_uv_state(st)
      st = _mat_slow_bsdf_state(st)
      st = _mat_slow_apply_aux_state(st)
   }
   def rec = _mat_slow_finalize_record(st)
   def pack_spec = bool(st.get("has_minfo", false)) ? st.get("spec", dict(0)) : dict(0)
   _mat_log_pack_summary(
      mi,
      pack_spec,
      bool(st.get("has_minfo", false)),
      int(st.get("base_id", -1)),
      int(st.get("met_rough_id", -1)),
      int(st.get("emissive_id", -1)),
      int(rec.get("ext2_tex_word", 0x80000000)),
      int(st.get("bsdf0_u32", 0)),
      int(st.get("bsdf2_u32", 0)),
      int(rec.get("bsdf4_u32", 0)),
   int(rec.get("bsdf5_u32", 0)))
   _mat_log_diag(
      mi,
      int(st.get("base_id", -1)),
      int(st.get("normal_id", -1)),
      int(st.get("met_rough_id", -1)),
      int(st.get("occlusion_id", -1)),
      int(st.get("emissive_id", -1)),
      int(st.get("base_color_u32", 0xffffffff)),
      int(st.get("alpha_u32", 0)),
      int(rec.get("material_u32", 0x0000ff00)),
   diag_on)
   return {"rec": rec, "loaded_count": int(st.get("loaded_count", 0))}
}

fn build_material_records(list material_infos, str base_path) list {
   "Builds GPU-ready material records from parsed glTF material info list."
   def diag_on = _scene_diag_enabled()
   def stage_trace = _scene_stage_trace_enabled()
   def mat_count = material_infos.len
   _scene_stage(stage_trace, "materials.begin count=" + to_str(mat_count) + " base=" + base_path)
   ;; Fast-core PBR is a load-time optimization.  Keep it opt-in for now because
   ;; some glTF sample models (Avocado-style texture sets) need the full material
   ;; path to keep base/normal/MR texture bindings identical across GL and VK.
   if common.env_truthy("NY_SCENE_FAST_CORE_PBR") && _mat_infos_fast_core_pbr_ok(material_infos) {
      def fast_records = _build_material_records_fast_core_pbr(material_infos, base_path, diag_on, stage_trace)
      _scene_stage(stage_trace, "materials.end count=" + to_str(fast_records.len) + " fast=core_pbr")
      return fast_records
   }
   mut mat_records = list(0)
   mut mat_specs = material_infos
   mut loaded_tex_count = 0
   def predecode = _mat_predecode_request_map(mat_specs, base_path, mat_count, stage_trace)
   def tex_req_map = predecode.get("req_map", dict(0))
   def predecode_active = bool(predecode.get("active", false))
   mut mi = 0
   while mi < mat_count {
      def built = _mat_slow_record_from_spec(mat_specs.get(mi, 0), tex_req_map, predecode_active, base_path, mi, diag_on)
      loaded_tex_count += int(built.get("loaded_count", 0))
      mat_records = mat_records.append(built.get("rec", dict(0)))
      mi += 1
   }
   if diag_on { terminal.log("[gltf] Loaded " + to_str(loaded_tex_count) + " textures for " + to_str(mat_count) + " materials") }
   _scene_stage(stage_trace, "materials.end count=" + to_str(mat_records.len))
   return mat_records
}

fn _packed_material_is_optical(int alpha_u32, int bsdf0_u32, int bsdf2_u32, int bsdf4_u32, int bsdf5_u32) bool {
   "Returns true when a material needs scene-color capture/refraction."
   if (int(alpha_u32) & 3) == 2 { return false }
   if ((int(bsdf0_u32)>> 16) & 255) > 0 { return true }
   if (int(bsdf5_u32) & 255) > 0 { return true }
   if ((int(bsdf5_u32)>> 8) & 255) > 0 { return true }
   false
}

fn _gpu_rec_is_optical(any rec) bool {
   if !is_list(rec)|| rec.len < 43 { return false }
   _packed_material_is_optical(
      int(rec.get(17, 0)),
      int(rec.get(20, 0)),
      int(rec.get(22, 0)),
      int(rec.get(41, 0)),
   int(rec.get(42, 0)))
}

fn _gpu_rec_fast_lit_ok(list rec, int alpha_u32, bool unlit, bool is_blend, bool is_optical) bool {
   if common.env_enabled("NY_GLTF_DISABLE_FAST_LIT") { return false }
   if !is_list(rec)|| rec.len < 46 { return false }
   if unlit || is_blend || is_optical { return false }
   if int(rec.get(0, -1)) >= 0 { return false }
   if int(rec.get(12, 0xffffffff)) != 0xffffffff { return false }
   if int(rec.get(14, -1)) >= 0 { return false }
   if int(rec.get(18, -1)) >= 0 { return false }
   def material_u32 = int(rec.get(13, 0x0000ff00))
   if material_u32 != 0x0000ff00 { return false }
   def mr_tex = band(bshr(material_u32, 16), 0x7fff)
   if mr_tex > 0 { return false }
   def normal_word = int(rec.get(24, 0x80000000))
   def normal_tid = band(normal_word, 0xffff)
   if normal_tid < MAX_TEXTURES { return false }
   if band(int(alpha_u32), 0xff000000) != 0 { return false }
   def bsdf0 = int(rec.get(20, 0))
   if band(bsdf0, 0xffffff00)!= 0 { return false }
   if int(rec.get(22, 0)) != 0 { return false }
   if int(rec.get(41, 0)) != 0 { return false }
   def bsdf5 = int(rec.get(42, 0))
   if band(bsdf5, 0x00ffffff)!= 0 { return false }
   def alpha_cov = band(bshr(bsdf5, 24), 255)
   if alpha_cov != 0 && alpha_cov != 255 { return false }
   if int(rec.get(43, 0x80000000)) != 0x80000000 { return false }
   if band(int(rec.get(24, 0)), 0x80000000) != 0 { return false }
   if band(int(rec.get(45, 0)), _SCENE_VC_VERTEX_TEX | _SCENE_VC_VERTEX_MATERIAL) != 0 { return false }
   if int(rec.get(45, 0)) == 2 { return false }
   true
}

fn _gpu_rec_pipeline_sort_key(any rec) int {
   if !is_list(rec)|| rec.len < 18 { return 0 }
   def flags = int(rec.get(8, 0))
   def is_lines = (flags & 1) != 0
   def is_points = (flags & 16) != 0
   def unlit = (flags & 2) != 0
   def nocull = (flags & 4) != 0
   def flip_winding = int(rec.get(44, 0)) != 0
   def alpha_u32 = int(rec.get(17, 0))
   def is_blend = (alpha_u32 & 3) == 2
   def is_optical = _gpu_rec_is_optical(rec)
   def pass = is_blend ? 2 : (is_optical ? 1 : 0)
   mut pipe_class = 0
   if is_lines {
      pipe_class = 80
   }
   elif is_points {
      pipe_class = 81
   }
   elif unlit {
      pipe_class = 30
   }
   elif _gpu_rec_fast_lit_ok(rec, alpha_u32, unlit, is_blend, is_optical) {
      pipe_class = 10
   }
   else { pipe_class = 20 }
   pass * 100000 + pipe_class * 100 + (nocull ? 2 : 0) + (flip_winding ? 1 : 0)
}

fn _cpu_part_is_optical(any part) bool {
   if !is_dict(part) { return false }
   def slab = part.get("material_slab", 0)
   if slab {
      return _packed_material_is_optical(
         load32(slab, 24),
         load32(slab, 36),
         load32(slab, 44),
         load32(slab, 144),
      load32(slab, 148))
   }
   def mesh = part.get("mesh", 0)
   if is_dict(mesh) {
      def mslab = mesh.get("material_slab", 0)
      if mslab {
         return _packed_material_is_optical(
            load32(mslab, 24),
            load32(mslab, 36),
            load32(mslab, 44),
            load32(mslab, 144),
         load32(mslab, 148))
      }
   }
   _packed_material_is_optical(
      int(part.get("alpha_u32", 0)),
      int(part.get("bsdf0_u32", 0)),
      int(part.get("bsdf2_u32", 0)),
      int(part.get("bsdf4_u32", 0)),
   int(part.get("bsdf5_u32", 0)))
}

fn _gpu_rec_pass_bucket(any rec) int {
   if !is_list(rec)|| rec.len < 18 { return 0 }
   if (int(rec.get(17, 0)) & 3) == 2 { return 2 }
   _gpu_rec_is_optical(rec) ? 1 : 0
}

fn _cpu_part_pass_bucket(any part) int {
   if !is_dict(part) { return 0 }
   if (int(part.get("alpha_u32", 0)) & 3) == 2 { return 2 }
   _cpu_part_is_optical(part) ? 1 : 0
}

fn _scene_sort_part_tex(any item, bool cpu_parts) int {
   cpu_parts ? int(item.get("tex_id", 0)) : int(item.get(0, 0))
}

fn _scene_sort_part_pipe(any item, bool cpu_parts) int {
   cpu_parts ? 0 : _gpu_rec_pipeline_sort_key(item)
}

fn _scene_sort_part_bucket(any item, bool cpu_parts) int {
   cpu_parts ? _cpu_part_pass_bucket(item) : _gpu_rec_pass_bucket(item)
}

fn _sort_scene_parts_by_blend_tex(any parts, bool cpu_parts=false) any {
   "Sorts part records: solid opaque first, optical opaque next, blend last, with opaque backdrops before foreground samples."
   if !is_list(parts) { return parts }
   if parts.len <= 1 { return parts }
   def part_count = parts.len
   mut i = 1
   while i < part_count {
      def cur = parts.get(i, 0)
      def cur_bucket = _scene_sort_part_bucket(cur, cpu_parts)
      def cur_backdrop = 0
      def cur_pipe = _scene_sort_part_pipe(cur, cpu_parts)
      def cur_tex = _scene_sort_part_tex(cur, cpu_parts)
      mut j = i - 1
      while j >= 0 {
         def prev = parts.get(j, 0)
         def prev_bucket = _scene_sort_part_bucket(prev, cpu_parts)
         def prev_backdrop = 0
         def prev_pipe = _scene_sort_part_pipe(prev, cpu_parts)
         def prev_tex = _scene_sort_part_tex(prev, cpu_parts)
         def move = (prev_bucket > cur_bucket) ||
         (prev_bucket == cur_bucket && (
               prev_backdrop < cur_backdrop ||
               (prev_backdrop == cur_backdrop && (prev_pipe > cur_pipe ||
         (prev_pipe == cur_pipe && prev_tex > cur_tex)))))
         if !move { break }
         parts[j + 1] = prev
         j -= 1
      }
      parts[j + 1] = cur
      i += 1
   }
   parts
}

fn _sort_gpu_parts_by_blend_tex(any gpu_parts) any {
   _sort_scene_parts_by_blend_tex(gpu_parts, false)
}

fn _sort_cpu_parts_by_blend_tex(any parts) any {
   _sort_scene_parts_by_blend_tex(parts, true)
}

fn _scene_part_center_world_depth2(any model_m, any minv, any maxv, f64 cam_x, f64 cam_y, f64 cam_z) f64 {
   "Returns squared camera distance of a part bounds center after model transform."
   def model = _as_render_model_mat4(model_m)
   def cx = (float(minv.get(0, 0.0)) + float(maxv.get(0, 0.0))) * 0.5
   def cy = (float(minv.get(1, 0.0)) + float(maxv.get(1, 0.0))) * 0.5
   def cz = (float(minv.get(2, 0.0)) + float(maxv.get(2, 0.0))) * 0.5
   def m00 = float(model.get(2, 1.0))
   def m10 = float(model.get(3, 0.0))
   def m20 = float(model.get(4, 0.0))
   def m01 = float(model.get(6, 0.0))
   def m11 = float(model.get(7, 1.0))
   def m21 = float(model.get(8, 0.0))
   def m02 = float(model.get(10, 0.0))
   def m12 = float(model.get(11, 0.0))
   def m22 = float(model.get(12, 1.0))
   def m03 = float(model.get(14, 0.0))
   def m13 = float(model.get(15, 0.0))
   def m23 = float(model.get(16, 0.0))
   def wx = m00 * cx + m01 * cy + m02 * cz + m03
   def wy = m10 * cx + m11 * cy + m12 * cz + m13
   def wz = m20 * cx + m21 * cy + m22 * cz + m23
   def dx = wx - cam_x
   def dy = wy - cam_y
   def dz = wz - cam_z
   dx * dx + dy * dy + dz * dz
}

fn _sort_gpu_parts_blend_camera(any gpu_parts, f64 cam_x, f64 cam_y, f64 cam_z) any {
   "Back-to-front sorts only the blend suffix of GPU part records."
   if !is_list(gpu_parts)|| gpu_parts.len <= 1 { return gpu_parts }
   def gpu_n = gpu_parts.len
   def blend_start = _gpu_parts_blend_start(gpu_parts)
   mut i = blend_start + 1
   while i < gpu_n {
      def cur = gpu_parts.get(i, 0)
      def cur_depth = _scene_part_center_world_depth2(
         cur.get(10, 0),
         cur.get(38, [0.0, 0.0, 0.0]),
         cur.get(39, [0.0, 0.0, 0.0]),
      cam_x, cam_y, cam_z)
      mut j = i - 1
      while j >= blend_start {
         def prev = gpu_parts.get(j, 0)
         def prev_depth = _scene_part_center_world_depth2(
            prev.get(10, 0),
            prev.get(38, [0.0, 0.0, 0.0]),
            prev.get(39, [0.0, 0.0, 0.0]),
         cam_x, cam_y, cam_z)
         if prev_depth >= cur_depth { break }
         gpu_parts[j + 1] = prev
         j -= 1
      }
      gpu_parts[j + 1] = cur
      i += 1
   }
   gpu_parts
}

fn _sort_cpu_parts_blend_camera(any parts, f64 cam_x, f64 cam_y, f64 cam_z) any {
   "Back-to-front sorts only the blend suffix of CPU part dicts."
   if !is_list(parts)|| parts.len <= 1 { return parts }
   def parts_n = parts.len
   mut blend_start = parts_n
   mut bi = 0
   while bi < parts_n {
      if _cpu_part_pass_bucket(parts.get(bi, 0)) == 2 {
         blend_start = bi
         break
      }
      bi += 1
   }
   mut i = blend_start + 1
   while i < parts_n {
      def cur = parts.get(i, 0)
      def cur_depth = _scene_part_center_world_depth2(
         cur.get("model", 0),
         cur.get("min", [0.0, 0.0, 0.0]),
         cur.get("max", [0.0, 0.0, 0.0]),
      cam_x, cam_y, cam_z)
      mut j = i - 1
      while j >= blend_start {
         def prev = parts.get(j, 0)
         def prev_depth = _scene_part_center_world_depth2(
            prev.get("model", 0),
            prev.get("min", [0.0, 0.0, 0.0]),
            prev.get("max", [0.0, 0.0, 0.0]),
         cam_x, cam_y, cam_z)
         if prev_depth >= cur_depth { break }
         parts[j + 1] = prev
         j -= 1
      }
      parts[j + 1] = cur
      i += 1
   }
   parts
}

fn _gpu_parts_first_bucket(any gpu_parts, int want_bucket, bool want_nonzero=false) int {
   if !is_list(gpu_parts) { return 0 }
   mut i = 0
   def n = gpu_parts.len
   while i < n {
      def bucket = _gpu_rec_pass_bucket(gpu_parts.get(i, 0))
      if want_nonzero ? bucket != 0 : bucket == want_bucket { return i }
      i += 1
   }
   n
}

fn _gpu_parts_blend_start(any gpu_parts) int {
   "Returns the first blend part index in a sorted GPU part list."
   _gpu_parts_first_bucket(gpu_parts, 2)
}

fn _gpu_parts_optical_start(any gpu_parts) int {
   "Returns the first non-solid GPU part index in a sorted part list."
   _gpu_parts_first_bucket(gpu_parts, 0, true)
}

fn _scene_alloc_zero_slab(int count, int stride) any {
   if count == 0 { return 0 }
   def slab = malloc(count * stride)
   if !slab { return 0 }
   memset(slab, 0, count * stride)
   slab
}

fn _pack_scene_lights_slab(
   list lights,
   f64 fit_scale=1.0,
   f64 fit_tx=0.0,
   f64 fit_ty=0.0,
   f64 fit_tz=0.0,
   bool scale_intensity=false
) any {
   "Packs punctual scene lights into a compact native slab.
   Format: [pos:12][color:12][intensity:4][range:4][type:4][dir:12][outer:4][pad:4] = 56 bytes per light."
   def count = is_list(lights) ? lights.len : 0
   def slab = _scene_alloc_zero_slab(count, 56)
   if !slab { return 0 }
   mut i = 0
   while i < count {
      def l = lights.get(i, 0)
      if is_dict(l) {
         def base = slab + (i * 56)
         def pos = l.get("position", [0.0, 0.0, 0.0])
         def dir = l.get("direction", [0.0, 0.0, -1.0])
         def col = l.get("color", [1.0, 1.0, 1.0])
         mut raw_intensity = float(l.get("intensity", 1.0))
         mut raw_range = float(l.get("range", 10.0))
         def ltype = to_str(l.get("type", "point"))
         def fit_scale_f = float(fit_scale)
         if scale_intensity && ltype != "directional" { raw_intensity = raw_intensity * fit_scale_f * fit_scale_f }
         if raw_range > 0.0 { raw_range = raw_range * fit_scale_f }
         def peak_col = max(float(col.get(0, 1.0)), max(float(col.get(1, 1.0)), float(col.get(2, 1.0))))
         def raw_peak = max(0.0, peak_col * raw_intensity)
         def peak_cap = (ltype == "directional" || raw_range <= 0.0) ? 8.0 : 128.0
         def visible_intensity = raw_peak > peak_cap ? (raw_intensity * (peak_cap / raw_peak)) : raw_intensity
         store32_f32(base, float(pos.get(0, 0.0)) * fit_scale_f + fit_tx, 0)
         store32_f32(base, float(pos.get(1, 0.0)) * fit_scale_f + fit_ty, 4)
         store32_f32(base, float(pos.get(2, 0.0)) * fit_scale_f + fit_tz, 8)
         store32_f32(base, float(col.get(0, 1.0)), 12)
         store32_f32(base, float(col.get(1, 1.0)), 16)
         store32_f32(base, float(col.get(2, 1.0)), 20)
         store32_f32(base, visible_intensity, 24)
         store32_f32(base, raw_range, 28)
         mut ltype_id = 1
         if ltype == "directional" { ltype_id = 0 } elif ltype == "spot" { ltype_id = 2 }
         store32(base, ltype_id, 32)
         store32_f32(base, float(dir.get(0, 0.0)), 36)
         store32_f32(base, float(dir.get(1, 0.0)), 40)
         store32_f32(base, float(dir.get(2, -1.0)), 44)
         store32_f32(base, float(l.get("outer_cone_cos", 0.0)), 48)
      }
      i += 1
   }
   slab
}

fn _scene_gpu_part_pipeline(bool is_lines, bool is_points, bool unlit, bool nocull, bool flip_winding, bool wants_alpha_pipe, bool fast_lit, bool no_punctual_lights) any {
   render.renderer_mesh_pipeline(is_lines, is_points, unlit, nocull, flip_winding, wants_alpha_pipe, fast_lit, no_punctual_lights)
}

fn _scene_store_gpu_part_header(any base, list rec, any pipe, bool is_lines, bool is_points) bool {
   store32(base, int(rec.get(0, -1)), 0)
   store32_f32(base, float(rec.get(1, 1.0)), 4)
   store64(base, pipe, 8)
   store64(base, rec.get(2, 0), 16)
   store64_h(base, rec.get(3, 0), 24)
   store64(base, rec.get(4, 0), 32)
   store64_h(base, rec.get(5, 0), 40)
   store32(base, int(rec.get(7, 0)), 48)
   store32(base, int(rec.get(6, 0)), 52)
   store32(base, int(rec.get(9, 0)), 56)
   store32(base, is_lines ? 1 : (is_points ? 2 : 0), 60)
   true
}

fn _scene_store_gpu_part_model(any base, any model_m) bool {
   def model_len = is_list(model_m) ? model_m.len : 0
   if model_len <= 0 { return false }
   if model_len == 18 && int(model_m.get(0, 0)) == 4 && int(model_m.get(1, 0)) == 4 {
      mut j = 0
      while j < 16 {
         store32_f32(base + 64, float(model_m.get(2 + j, 0.0)), j * 4)
         j += 1
      }
      return true
   }
   if model_len == 16 {
      mut j = 0
      while j < 16 {
         store32_f32(base + 64, float(model_m.get(j, 0.0)), j * 4)
         j += 1
      }
      return true
   }
   false
}

fn _scene_gpu_rec_mat_idx(list rec) int {
   if rec.len > 47 { return int(rec.get(47, -1)) }
   def rec_mesh = rec.get(40, 0)
   is_dict(rec_mesh) ? int(rec_mesh.get("mat_idx", rec_mesh.get("material_idx", -1))) : -1
}

fn _scene_store_gpu_part_material(any base, list rec, bool unlit, bool is_points, bool is_lines) bool {
   def rec_mat_idx = _scene_gpu_rec_mat_idx(rec)
   store32(base, rec_mat_idx >= 0 ? rec_mat_idx + 1 : 0, 128)
   store32(base, (unlit || is_points || is_lines) ? 1 : 0, 132)
   ;; Keep this layout byte-for-byte aligned with vk.renderer
   ;; _set_material_from_part_slab(base + 136).  A previous version stored
   ;; emissive_tex/emissive_u32 in the opposite order and placed bsdf4/bsdf5
   ;; four bytes early, which made Vulkan read stale material words.  That
   ;; showed up as glTF base-color textures becoming gray/striped even though
   ;; mesh UVs and asset parsing were correct.
   store32(base, int(rec.get(12, 0xffffffff)), 136) ;; p+0   base_color_u32
   store32(base, int(rec.get(13, 0x0000ff00)), 140) ;; p+4   material_u32
   store32(base, int(rec.get(15, 0)), 144) ;; p+8   emissive_u32
   store32(base, int(rec.get(14, -1)), 148) ;; p+12  emissive_tex_id
   store32(base, int(rec.get(16, 0)), 152) ;; p+16  emissive_uv_set
   store32(base, int(rec.get(0, -1)), 156) ;; p+20  base_tex_id
   store32(base, int(rec.get(17, 0)), 160) ;; p+24  alpha_u32
   store32(base, int(rec.get(18, -1)), 164) ;; p+28  occlusion_tex_id
   store32(base, int(rec.get(19, 0)), 168) ;; p+32  occlusion_uv_set
   store32(base, int(rec.get(20, 0)), 172) ;; p+36  bsdf0_u32
   store32(base, int(rec.get(21, 0)), 176) ;; p+40  bsdf1_u32
   store32(base, int(rec.get(22, 0)), 180) ;; p+44  bsdf2_u32
   store32(base, int(rec.get(23, 0)), 184) ;; p+48  bsdf3_u32
   store32(base, int(rec.get(26, 0)), 188) ;; p+52  base_uv_xf0
   store32(base, int(rec.get(27, 0)), 192) ;; p+56  base_uv_xf1
   store32(base, int(rec.get(28, 0)), 196) ;; p+60  normal_uv_xf0
   store32(base, int(rec.get(29, 0)), 200) ;; p+64  normal_uv_xf1
   store32(base, int(rec.get(30, 0)), 204) ;; p+68  mr_uv_xf0
   store32(base, int(rec.get(31, 0)), 208) ;; p+72  mr_uv_xf1
   store32(base, int(rec.get(32, 0)), 212) ;; p+76  occlusion_uv_xf0
   store32(base, int(rec.get(33, 0)), 216) ;; p+80  occlusion_uv_xf1
   store32(base, int(rec.get(34, 0)), 220) ;; p+84  emissive_uv_xf0
   store32(base, int(rec.get(35, 0)), 224) ;; p+88  emissive_uv_xf1
   store32(base, 0, 228) ;; p+92  reserved
   store32(base, int(rec.get(41, 0)), 232) ;; p+96  bsdf4_u32
   store32(base, int(rec.get(24, -1)), 236) ;; p+100 normal_tex_word
   store32(base, int(rec.get(42, 0)), 240) ;; p+104 bsdf5_u32
   store32(base, int(rec.get(43, 0x80000000)), 244) ;; p+108 ext2_tex_word
   store32(base, int(rec.get(45, 0)), 248) ;; p+112 vc_mode
   true
}

fn _scene_trace_gpu_part_slab(any base, list rec, int i, bool trace_slab) bool {
   if !trace_slab || i >= 4 { return false }
   ui_profile.print_text("[scene:slab] i=" + to_str(i) +
      " rec0=" + to_str(int(rec.get(0, -1))) +
      " rec12=0x" + str.to_hex(int(rec.get(12, 0xffffffff))) +
      " rec13=0x" + str.to_hex(int(rec.get(13, 0x0000ff00))) +
      " rec24=0x" + str.to_hex(int(rec.get(24, -1))) +
      " slab0=" + to_str(load32_h(base, 0)) +
      " matKey=" + to_str(load32_h(base, 128)) +
      " slabTex=" + to_str(load32_h(base, 156)) +
      " slabMat=0x" + str.to_hex(load32_h(base, 140)) +
      " slabBsdf4=0x" + str.to_hex(load32_h(base, 232)) +
      " slabBsdf5=0x" + str.to_hex(load32_h(base, 240)) +
   " slabNormal=0x" + str.to_hex(load32_h(base, 236)))
   true
}

fn _pack_scene_gpu_parts_slab(list parts, bool no_punctual_lights=false) any {
   "Packs a list of GPU part records into a compact native slab."
   def count = parts.len
   def slab = _scene_alloc_zero_slab(count, 256)
   if !slab { return 0 }
   def trace_pipe = common.env_enabled("NY_RENDER_PIPELINE_TRACE") || common.env_enabled("NY_VK_PIPELINE_TRACE")
   def trace_slab = common.env_enabled("NY_UI_SLAB_TRACE")
   mut trace_fast = 0
   mut trace_fast_env = 0
   mut trace_full = 0
   mut trace_unlit = 0
   mut trace_blend = 0
   mut trace_optical = 0
   mut trace_special = 0
   mut trace_nocull = 0
   mut trace_flip = 0
   mut i = 0
   while i < count {
      def rec = parts.get(i, 0)
      if is_list(rec) && rec.len >= 41 {
         def base = slab + (i * 256)
         def flags = int(rec.get(8, 0))
         def is_lines = (flags & 1) != 0
         def is_points = (flags & 16) != 0
         def unlit = (flags & 2) != 0
         def nocull = (flags & 4) != 0
         def flip_winding = int(rec.get(44, 0)) != 0
         def double_sided = int(rec.get(46, 0)) != 0
         def alpha_u32 = int(rec.get(17, 0))
         def is_blend = (alpha_u32 & 3) == 2
         def is_optical = _gpu_rec_is_optical(rec)
         def wants_alpha_pipe = is_blend
         def fast_lit = _gpu_rec_fast_lit_ok(rec, alpha_u32, unlit, is_blend, is_optical)
         if trace_pipe {
            if nocull { trace_nocull += 1 }
            if flip_winding { trace_flip += 1 }
            if is_lines || is_points {
               trace_special += 1
            }
            elif is_blend {
               trace_blend += 1
            }
            elif is_optical {
               trace_optical += 1
            }
            elif unlit {
               trace_unlit += 1
            }
            elif fast_lit && no_punctual_lights {
               trace_fast_env += 1
            }
            elif fast_lit {
               trace_fast += 1
            }
            else { trace_full += 1 }
         }
         def pipe = _scene_gpu_part_pipeline(is_lines, is_points, unlit, nocull, flip_winding, wants_alpha_pipe, fast_lit, no_punctual_lights)
         _scene_store_gpu_part_header(base, rec, pipe, is_lines, is_points)
         _scene_store_gpu_part_model(base, rec.get(10, 0))
         def rec_node_idx = int(rec.get(11, -1))
         _scene_store_gpu_part_material(base, rec, unlit, is_points, is_lines)
         _scene_trace_gpu_part_slab(base, rec, i, trace_slab)
         def is_instanced_rec = int(rec.get(36, 0)) != 0
         store32(base, (!is_instanced_rec && rec_node_idx >= 0) ? rec_node_idx + 1 : 0, 252)
      }
      i += 1
   }
   if trace_pipe {
      terminal.log("[gpu:pipe] parts=" + to_str(count) +
         " fast_env=" + to_str(trace_fast_env) +
         " fast=" + to_str(trace_fast) +
         " full=" + to_str(trace_full) +
         " unlit=" + to_str(trace_unlit) +
         " optical=" + to_str(trace_optical) +
         " blend=" + to_str(trace_blend) +
         " special=" + to_str(trace_special) +
         " nocull=" + to_str(trace_nocull) +
      " flip=" + to_str(trace_flip))
   }
   slab
}

fn _scene_part_index_u32(any part) bool {
   def opts = part.get("opts", 0)
   part.get("index_type_u32", (is_dict(opts) && opts.get("index_type_u32", false))) ? true : false
}

fn _scene_part_opt_bool(any part, str key, bool fallback=false) bool {
   if key == "is_points" && int(part.get("primitive_mode", 4)) == 0 { return true }
   if key == "is_lines" {
      def prim_mode = int(part.get("primitive_mode", 4))
      if prim_mode == 1 || prim_mode == 2 || prim_mode == 3 { return true }
   }
   def opts = part.get("opts", 0)
   if part.get(key, false) { return true }
   if is_dict(opts) { return opts.get(key, fallback) ? true : false }
   fallback ? true : false
}

fn _scene_part_bool_field(any part, str key, bool fallback=false) bool {
   part.get(key, fallback) ? true : false
}

fn _scene_merge_diag_enabled() bool {
   _scene_force_group_diag_enabled() || _scene_merge_trace_enabled()
}

fn _scene_merge_sort_enabled() bool {
   common.env_toggle("NY_UI_MERGE_SORT", true)
}

fn _scene_shallow_copy_parts(any parts) list {
   mut out = list(0)
   if !is_list(parts) { return out }
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      out = out.append(parts.get(i, 0))
      i += 1
   }
   out
}

fn _scene_dict_like(any v) bool {
   is_dict(v) || type(v) == "dict"
}

fn _scene_list_any_safe(any xs, int idx, any fallback=0) any {
   if is_list(xs)&& idx >= 0 && idx < xs.len { return xs.get(idx, fallback) }
   fallback
}

fn _scene_list_int_safe(any xs, int idx, int fallback=0) int {
   int(_scene_list_any_safe(xs, idx, fallback))
}

fn _scene_list_num_safe(any xs, int idx, f64 fallback=0.0) f64 {
   float(_scene_list_any_safe(xs, idx, fallback))
}

fn _scene_part_same_i32(any a, any b, str key, any fallback=0) bool {
   int(a.get(key, fallback)) == int(b.get(key, fallback))
}

fn _scene_part_i32_for_key(any part, str key, any fallback=0) int {
   int(part.get(key, fallback))
}

fn _scene_part_bool_for_merge(any part, str key) int {
   _scene_part_opt_bool(part, key, false) ? 1 : 0
}

fn _scene_part_normal_word(any part) int {
   int(part.get("normal_tex_word", render_utils.pack_normal_tex_word(int(part.get("normal_tex_id",
            -1)),
            int(part.get("normal_uv_set",
   0)))))
}

fn _scene_parts_same_normal_word(any first, any next) bool {
   _scene_part_same_i32(first, next, "normal_tex_word", _scene_part_normal_word(first))
}

fn _scene_parts_same_merge_bool_fields(any first, any next) bool {
   _scene_parts_same_opt_bool_fields(first, next, _SCENE_MERGE_OPT_BOOL_FIELDS)
}

fn _scene_part_merge_less(any a, any b, f64 scene_area_like) bool {
   if !is_dict(a) { return false }
   if !is_dict(b) { return true }
   def pass_a, pass_b = _cpu_part_pass_bucket(a), _cpu_part_pass_bucket(b)
   if pass_a != pass_b { return pass_a < pass_b }
   def backdrop_a = _scene_part_is_backdrop(a, scene_area_like) ? 1 : 0
   def backdrop_b = _scene_part_is_backdrop(b, scene_area_like) ? 1 : 0
   if backdrop_a != backdrop_b {
      return backdrop_a > backdrop_b
   }
   def idx_a, idx_b = _scene_part_index_u32(a) ? 1 : 0, _scene_part_index_u32(b) ? 1 : 0
   if idx_a != idx_b { return idx_a < idx_b }
   mut i = 0
   def fields = _scene_merge_i32_field_defaults()
   def fields_n = fields.len
   while i < fields_n {
      def row = fields.get(i, [])
      def key = to_str(row.get(0, ""))
      def fallback = row.get(1, 0)
      def va = _scene_part_i32_for_key(a, key, fallback)
      def vb = _scene_part_i32_for_key(b, key, fallback)
      if va != vb { return va < vb }
      i += 1
   }
   def nwa, nwb = _scene_part_normal_word(a), _scene_part_normal_word(b)
   if nwa != nwb { return nwa < nwb }
   i = 0
   def opt_n = _SCENE_MERGE_OPT_BOOL_FIELDS.len
   while i < opt_n {
      def key = to_str(_SCENE_MERGE_OPT_BOOL_FIELDS.get(i, ""))
      def va = _scene_part_bool_for_merge(a, key)
      def vb = _scene_part_bool_for_merge(b, key)
      if va != vb { return va < vb }
      i += 1
   }
   def visa = _scene_part_bool_field(a, "visible", true) ? 1 : 0
   def visb = _scene_part_bool_field(b, "visible", true) ? 1 : 0
   if visa != visb { return visa < visb }
   def vc_a, vc_b = int(a.get("vcnt", 0)), int(b.get("vcnt", 0))
   if vc_a != vc_b { return vc_a > vc_b }
   false
}

fn _sort_scene_parts_for_merge(any parts, f64 scene_area_like) any {
   if !is_list(parts)|| parts.len <= 2 { return parts }
   def parts_n = parts.len
   mut i = 1
   while i < parts_n {
      def cur = parts.get(i, 0)
      mut j = i - 1
      while j >= 0 {
         def prev = parts.get(j, 0)
         if !_scene_part_merge_less(cur, prev, scene_area_like) { break }
         parts[j + 1] = prev
         j -= 1
      }
      parts[j + 1] = cur
      i += 1
   }
   parts
}

fn _scene_parts_same_i32_fields(any a, any b, any field_defaults) bool {
   mut i = 0
   def fields_n = field_defaults.len
   while i < fields_n {
      def row = field_defaults.get(i, [])
      if !_scene_part_same_i32(a, b, to_str(row.get(0, "")), row.get(1, 0)) { return false }
      i += 1
   }
   true
}

fn _scene_parts_same_opt_bool_fields(any a, any b, any fields) bool {
   mut i = 0
   def fields_n = fields.len
   while i < fields_n {
      def key = to_str(fields.get(i, ""))
      if _scene_part_opt_bool(a, key, false)!= _scene_part_opt_bool(b, key, false) { return false }
      i += 1
   }
   true
}

fn _scene_part_same_model(any a, any b) bool {
   def ma, mb = _as_render_model_mat4(a.get("model", 0)), _as_render_model_mat4(b.get("model", 0))
   mut i = 2
   while i < 18 {
      if abs(float(ma.get(i, 0.0)) - float(mb.get(i, 0.0))) > 0.00001 { return false }
      i += 1
   }
   true
}

fn _scene_model_affine_bakeable(any m) bool {
   if !is_list(m)|| m.len < 18 { return false }
   def eps = 0.0000001
   if abs(float(m.get(5, 0.0))) > eps || abs(float(m.get(9, 0.0))) > eps || abs(float(m.get(13, 0.0))) > eps { return false }
   if abs(float(m.get(17, 1.0)) - 1.0) > eps { return false }
   mut i = 2
   while i < 18 {
      def v = float(m.get(i, 0.0))
      if is_nan(v)|| is_inf(v) || abs(v) > 1000000.0 { return false }
      i += 1
   }
   def m00, m10 = float(m.get(2, 1.0)), float(m.get(3, 0.0))
   def m20 = float(m.get(4, 0.0))
   def m01 = float(m.get(6, 0.0))
   def m11 = float(m.get(7, 1.0))
   def m21 = float(m.get(8, 0.0))
   def m02 = float(m.get(10, 0.0))
   def m12 = float(m.get(11, 0.0))
   def m22 = float(m.get(12, 1.0))
   def det = m00 * (m11 * m22 - m12 * m21) -
   m01 * (m10 * m22 - m12 * m20) +
   m02 * (m10 * m21 - m11 * m20)
   abs(det) > eps
}

fn _scene_part_model_bakeable(any part) bool {
   def m = _as_render_model_mat4(part.get("model", 0))
   _scene_model_affine_bakeable(m)
}

fn _scene_part_indices_valid(any part) bool {
   def iptr = part.get("iptr", 0)
   def icnt = int(part.get("icnt", 0))
   def vcnt = int(part.get("vcnt", 0))
   if !iptr || icnt <= 0 || vcnt <= 0 { return false }
   def idx_u32 = _scene_part_index_u32(part)
   mut i = 0
   while i < icnt {
      def vi = idx_u32 ? load32(iptr, i * 4) : load16(iptr, i * 2)
      if vi < 0 || vi >= vcnt { return false }
      i += 1
   }
   true
}

fn _scene_parts_merge_compatible(any first, any next, int total_vcnt, f64 scene_area_like) bool {
   if !is_dict(first)|| !is_dict(next) { return false }
   if _cpu_part_pass_bucket(first)!= 0 || _cpu_part_pass_bucket(next) != 0 { return false }
   if !first.get("vptr", 0)|| !next.get("vptr", 0) { return false }
   if !first.get("iptr", 0)|| !next.get("iptr", 0) { return false }
   def next_vcnt, next_icnt = int(next.get("vcnt", 0)), int(next.get("icnt", 0))
   if next_vcnt <= 0 || next_icnt <= 0 { return false }
   def idx_u32 = _scene_part_index_u32(first)
   if idx_u32 != _scene_part_index_u32(next) { return false }
   if !idx_u32 && total_vcnt > 65535 { return false }
   if int(first.get("skin_idx", -1)) >= 0 || int(next.get("skin_idx", -1)) >= 0 { return false }
   if _scene_part_is_backdrop(first, scene_area_like)!= _scene_part_is_backdrop(next, scene_area_like) { return false }
   if !_scene_part_same_model(first, next) {
      if !common.env_enabled("NY_UI_MERGE_BAKE_MODEL") { return false }
      if !_scene_part_model_bakeable(first)|| !_scene_part_model_bakeable(next) { return false }
   }
   if !_scene_parts_same_i32_fields(first, next, _scene_merge_i32_field_defaults()) { return false }
   if !_scene_parts_same_normal_word(first, next) { return false }
   if _scene_part_bool_field(first, "visible", true)!= _scene_part_bool_field(next, "visible", true) { return false }
   if !_scene_parts_same_merge_bool_fields(first, next) { return false }
   _scene_part_indices_valid(next)
}

fn _scene_source_part_cpu_resource(any part) dict {
   {"ptr": part.get("vptr", 0), "idx_ptr": part.get("iptr", 0)}
}

fn _scene_store_vertex_attrs(any vptr, int vcnt, int tex_id, int material_u32=0, bool set_material=false) bool {
   if !vptr || vcnt <= 0 { return false }
   mut vi = 0
   while vi < vcnt {
      def vv = vptr + vi * VERTEX_STRIDE
      if set_material { store32(vv, material_u32, _VKR_OFF_C) }
      store32(vv, tex_id, _VKR_OFF_TEX)
      vi += 1
   }
   true
}

fn _scene_copy_indices_u32(any iout, int iwrite, any iptr, int icnt, int vcnt, int vbase, bool idx_u32, bool validate=false) bool {
   mut ii = 0
   while ii < icnt {
      def local_vi = idx_u32 ? load32(iptr, ii * 4) : load16(iptr, ii * 2)
      if validate && (local_vi < 0 || local_vi >= vcnt) { return false }
      store32(iout, local_vi + vbase, (iwrite + ii) * 4)
      ii += 1
   }
   true
}

fn _scene_merge_part_summary(any part, f64 scene_area_like) str {
   if !is_dict(part) { return "<not-dict>" }
   def opts = part.get("opts", 0)
   "vcnt=" + to_str(int(part.get("vcnt", 0))) +
   " icnt=" + to_str(int(part.get("icnt", 0))) +
   " idx32=" + to_str(_scene_part_index_u32(part)) +
   " skin=" + to_str(int(part.get("skin_idx", -1))) +
   " inst=" + to_str(_scene_part_bool_field(part, "instanced_part", false)) +
   " backdrop=" + to_str(_scene_part_is_backdrop(part, scene_area_like)) +
   " bake=" + to_str(_scene_part_model_bakeable(part)) +
   " prim=" + to_str(int(part.get("primitive_mode", 4))) +
   " mat=" + to_str(int(part.get("mat_idx", -1))) +
   " tex=" + to_str(int(part.get("tex_id", -1))) +
   " color=0x" + str.to_hex(int(part.get("base_color_u32", 0xffffffff))) +
   " material=0x" + str.to_hex(int(part.get("material_u32", 0x0000ff00))) +
   " normal=" + to_str(int(part.get("normal_tex_id", -1))) +
   " normal_word=0x" + str.to_hex(int(part.get("normal_tex_word", render_utils.pack_normal_tex_word(int(part.get("normal_tex_id", -1)), int(part.get("normal_uv_set", 0)))))) +
   " bsdf0=0x" + str.to_hex(int(part.get("bsdf0_u32", 0))) +
   " bsdf1=0x" + str.to_hex(int(part.get("bsdf1_u32", 0))) +
   " ext2=0x" + str.to_hex(int(part.get("ext2_tex_word", 0x80000000))) +
   " vc=" + to_str(int(part.get("vc_mode", 0))) +
   " vis=" + to_str(_scene_part_bool_field(part, "visible", true)) +
   " lines=" + to_str(_scene_part_opt_bool(part, "is_lines", false)) +
   " points=" + to_str(_scene_part_opt_bool(part, "is_points", false)) +
   " unlit=" + to_str(_scene_part_opt_bool(part, "unlit", false)) +
   " no_cull=" + to_str(_scene_part_opt_bool(part, "no_cull", false)) +
   " double=" + to_str(_scene_part_opt_bool(part, "double_sided", false)) +
   " flip=" + to_str(_scene_part_opt_bool(part, "flip_winding", false))
}

fn _scene_free_two(any a, any b) int {
   if a { free(a) }
   if b { free(b) }
   0
}

fn _scene_merge_log_fail(str reason, int start_idx, int end_idx, int total_vcnt, int total_icnt) bool {
   if !_scene_merge_diag_enabled() { return false }
   ui_profile.print_text("[group:merge:build_fail] reason=" + reason +
      " start=" + to_str(start_idx) +
      " end=" + to_str(end_idx) +
      " total_v=" + to_str(total_vcnt) +
   " total_i=" + to_str(total_icnt))
   true
}

fn _scene_merge_run_same_model(list parts, int start_idx, int end_idx) bool {
   def first_part = parts.get(start_idx, 0)
   mut i = start_idx + 1
   while i < end_idx {
      if !_scene_part_same_model(first_part, parts.get(i, 0)) { return false }
      i += 1
   }
   true
}

fn _scene_merge_copy_part(
   any part,
   int pi,
   any vout,
   any iout,
   int vbase,
   int iwrite,
   bool idx_u32,
   bool run_same_model,
   any bounds_acc
) dict {
   def vptr = part.get("vptr", 0)
   def iptr = part.get("iptr", 0)
   def vcnt = int(part.get("vcnt", 0))
   def icnt = int(part.get("icnt", 0))
   if !vptr || !iptr || vcnt <= 0 || icnt <= 0 {
      if _scene_merge_diag_enabled() {
         ui_profile.print_text("[group:merge:build_fail] reason=part_input pi=" + to_str(pi) +
            " vptr=" + to_str(vptr != 0) +
            " iptr=" + to_str(iptr != 0) +
            " vcnt=" + to_str(vcnt) +
         " icnt=" + to_str(icnt))
      }
      return {"ok": false, "vbase": vbase, "iwrite": iwrite, "bounds_acc": bounds_acc}
   }
   memcpy(vout + vbase * VERTEX_STRIDE, vptr, vcnt * VERTEX_STRIDE)
   if !run_same_model
   && !_bake_vertex_buffer_model(vout + vbase * VERTEX_STRIDE, vcnt, _as_render_model_mat4(part.get("model", 0))){
      if _scene_merge_diag_enabled() {
         ui_profile.print_text("[group:merge:build_fail] reason=model_bake pi=" + to_str(pi) +
            " model_type=" + type(part.get("model", 0)) +
         " model_len=" + to_str(is_list(part.get("model", 0)) ? part.get("model", 0).len : -1))
      }
      return {"ok": false, "vbase": vbase, "iwrite": iwrite, "bounds_acc": bounds_acc}
   }
   mut write_i = iwrite
   mut ii = 0
   while ii < icnt {
      def vi = idx_u32 ? load32(iptr, ii * 4) : load16(iptr, ii * 2)
      if vi < 0 || vi >= vcnt {
         if _scene_merge_diag_enabled() {
            ui_profile.print_text("[group:merge:build_fail] reason=index pi=" + to_str(pi) +
               " ii=" + to_str(ii) +
               " vi=" + to_str(vi) +
            " vcnt=" + to_str(vcnt))
         }
         return {"ok": false, "vbase": vbase, "iwrite": write_i, "bounds_acc": bounds_acc}
      }
      def merged_vi = vi + vbase
      if idx_u32 {
         store32(iout, merged_vi, write_i * 4)
      } else {
         store16(iout, merged_vi, write_i * 2)
      }
      write_i += 1
      ii += 1
   }
   def bounds = run_same_model ? 0 : _scene_bounds_from_vertex_ptr(vout + vbase * VERTEX_STRIDE, vcnt)
   return {"ok": true, "vbase": vbase + vcnt, "iwrite": write_i, "bounds_acc": _scene_bounds_accum_part(bounds_acc, part, bounds)}
}

fn _scene_merge_finalize_indexed_part(list parts, int start_idx, int end_idx, int total_vcnt, int total_icnt, any vout, any iout, bool run_same_model, any merged_bounds) any {
   def first_part = parts.get(start_idx, 0)
   mut merged = first_part
   merged["vptr"] = vout
   merged["vcnt"] = total_vcnt
   merged["iptr"] = iout
   merged["icnt"] = total_icnt
   merged["model"] = run_same_model ? first_part.get("model", _identity_model_mat4()) : _identity_model_mat4()
   merged["node_idx"] = run_same_model ? int(first_part.get("node_idx", -1)) : -1
   if !run_same_model { merged["instanced_part"] = false }
   merged["min"] = merged_bounds.get(0, [0.0, 0.0, 0.0])
   merged["max"] = merged_bounds.get(1, [0.0, 0.0, 0.0])
   merged["merged_parts"] = end_idx - start_idx
   merged
}

fn _scene_merge_indexed_part_run(
   list parts,
   int start_idx,
   int end_idx,
   int total_vcnt,
   int total_icnt,
   bool idx_u32
) any {
   if end_idx <= start_idx + 1 || total_vcnt <= 0 || total_icnt <= 0 {
      _scene_merge_log_fail("range", start_idx, end_idx, total_vcnt, total_icnt)
      return 0
   }
   def run_same_model = _scene_merge_run_same_model(parts, start_idx, end_idx)
   def vout, iout = malloc(total_vcnt * VERTEX_STRIDE), malloc(total_icnt * (idx_u32 ? 4 : 2))
   if !vout || !iout {
      _scene_merge_log_fail("alloc", start_idx, end_idx, total_vcnt, total_icnt)
      return _scene_free_two(vout, iout)
   }
   mut vbase = 0
   mut iwrite = 0
   mut bounds_acc = _scene_bounds_accum_new()
   mut pi = start_idx
   while pi < end_idx {
      def copy_state = _scene_merge_copy_part(parts.get(pi, 0), pi, vout, iout, vbase, iwrite, idx_u32, run_same_model, bounds_acc)
      if !bool(copy_state.get("ok", false)) { return _scene_free_two(vout, iout) }
      vbase = int(copy_state.get("vbase", vbase))
      iwrite = int(copy_state.get("iwrite", iwrite))
      bounds_acc = copy_state.get("bounds_acc", bounds_acc)
      pi += 1
   }
   def merged_bounds = _scene_bounds_accum_result(bounds_acc)
   _scene_merge_finalize_indexed_part(parts, start_idx, end_idx, total_vcnt, total_icnt, vout, iout, run_same_model, merged_bounds)
}

fn _merge_scene_static_indexed_parts(any parts, f64 scene_area_like) dict {
   mut out = list(0)
   mut source_resources = list(0)
   mut merge_count = 0
   if _scene_merge_diag_enabled() { ui_profile.print_text("[group:merge:begin] parts=" + to_str(is_list(parts) ? parts.len : -1)) }
   if !is_list(parts) || parts.len <= 1 { return {"parts": parts, "source_resources": source_resources, "merge_count": 0} }
   if _scene_merge_sort_enabled() { parts = _sort_scene_parts_for_merge(_scene_shallow_copy_parts(parts), scene_area_like) }
   def parts_n = parts.len
   mut i = 0
   while i < parts_n {
      def first = parts.get(i, 0)
      mut run_end = i + 1
      mut total_vcnt = int(first.get("vcnt", 0))
      mut total_icnt = int(first.get("icnt", 0))
      def idx_u32 = _scene_part_index_u32(first)
      if is_dict(first)
      && first.get("vptr", 0)
      && first.get("iptr", 0)
      && total_vcnt > 0
      && total_icnt > 0
      && _scene_part_indices_valid(first){
         while run_end < parts_n {
            def next = parts.get(run_end, 0)
            def next_vcnt = int(next.get("vcnt", 0))
            def next_icnt = int(next.get("icnt", 0))
            if !_scene_parts_merge_compatible(first, next, total_vcnt + next_vcnt, scene_area_like) {
               if _scene_merge_diag_enabled() {
                  ui_profile.print_text("[group:merge:block] run=" + to_str(i) + " next=" + to_str(run_end) +
                     " model_same=" + to_str(_scene_part_same_model(first, next)) +
                     " first={" + _scene_merge_part_summary(first, scene_area_like) + "}" +
                  " next={" + _scene_merge_part_summary(next, scene_area_like) + "}")
               }
               break
            }
            total_vcnt += next_vcnt
            total_icnt += next_icnt
            run_end += 1
         }
      } elif _scene_merge_diag_enabled() {
         ui_profile.print_text("[group:merge:skip] run=" + to_str(i) +
            " dict=" + to_str(is_dict(first)) +
            " vptr=" + to_str(is_dict(first) && first.get("vptr", 0) != 0) +
            " iptr=" + to_str(is_dict(first) && first.get("iptr", 0) != 0) +
            " vcnt=" + to_str(total_vcnt) +
            " icnt=" + to_str(total_icnt) +
         " indices=" + to_str(is_dict(first) && _scene_part_indices_valid(first)))
      }
      if run_end > i + 1 {
         mut run_sources = list(0)
         mut ri = i
         while ri < run_end {
            run_sources = run_sources.append(_scene_source_part_cpu_resource(parts.get(ri, 0)))
            ri += 1
         }
         def merged = _scene_merge_indexed_part_run(parts, i, run_end, total_vcnt, total_icnt, idx_u32)
         if _scene_dict_like(merged) {
            out = out.append(merged)
            merge_count += (run_end - i - 1)
            ri = 0
            while ri < run_sources.len {
               source_resources = source_resources.append(run_sources.get(ri, 0))
               ri += 1
            }
            i = run_end
            continue
         } elif _scene_merge_diag_enabled() {
            ui_profile.print_text("[group:merge:failed] run=" + to_str(i) +
               " end=" + to_str(run_end) +
               " total_v=" + to_str(total_vcnt) +
               " total_i=" + to_str(total_icnt) +
               " idx32=" + to_str(idx_u32) +
               " ret_type=" + type(merged) +
            " is_dict=" + to_str(is_dict(merged)))
         }
      }
      out = out.append(first)
      i += 1
   }
   {"parts": out, "source_resources": source_resources, "merge_count": merge_count}
}

fn _scene_pack_upload_candidate(any parts) bool {
   if common.env_enabled("NY_GLTF_DISABLE_PACKED_UPLOAD") { return false }
   if !is_list(parts) { return false }
   def parts_n = parts.len
   def min_parts = common.env_int_clamped("NY_GLTF_PACK_UPLOAD_MIN_PARTS", 64, 2, 4096)
   if parts_n < min_parts { return false }
   mut total_v, total_i = 0, 0
   mut i = 0
   while i < parts_n {
      def part = parts.get(i, 0)
      if !is_dict(part) { return false }
      def vptr, iptr = part.get("vptr", 0), part.get("iptr", 0)
      def vcnt, icnt = int(part.get("vcnt", 0)), int(part.get("icnt", 0))
      if !vptr || !iptr || vcnt <= 0 || icnt <= 0 { return false }
      if !_scene_part_indices_valid(part) { return false }
      if int(part.get("primitive_mode", 4)) != 4 { return false }
      if int(part.get("skin_idx", -1)) >= 0 { return false }
      if _scene_part_bool_field(part, "instanced_part", false) { return false }
      total_v += vcnt
      total_i += icnt
      if total_v <= 0 || total_i <= 0 || total_v > 2140000000 || total_i > 2140000000 { return false }
      i += 1
   }
   parts_n >= min_parts || total_v > 65535
}

fn _scene_part_opt_bit(any part, any opts, str key) int {
   (part.get(key, false) || (is_dict(opts) && opts.get(key, false))) ? 1 : 0
}

fn _scene_part_bsdf_same_as(any part, any first) bool {
   if int(part.get("bsdf0_u32", 0)) != int(first.get("bsdf0_u32", 0)) { return false }
   if int(part.get("bsdf1_u32", 0)) != int(first.get("bsdf1_u32", 0)) { return false }
   if int(part.get("bsdf2_u32", 0)) != int(first.get("bsdf2_u32", 0)) { return false }
   if int(part.get("bsdf3_u32", 0)) != int(first.get("bsdf3_u32", 0)) { return false }
   if int(part.get("bsdf4_u32", 0)) != int(first.get("bsdf4_u32", 0)) { return false }
   int(part.get("bsdf5_u32", 0)) == int(first.get("bsdf5_u32", 0))
}

fn _scene_part_uv_xforms_zero(any part) bool {
   if int(part.get("base_uv_xf0", 0)) != 0 || int(part.get("base_uv_xf1", 0)) != 0 { return false }
   if int(part.get("normal_uv_xf0", 0)) != 0 || int(part.get("normal_uv_xf1", 0)) != 0 { return false }
   if int(part.get("mr_uv_xf0", 0)) != 0 || int(part.get("mr_uv_xf1", 0)) != 0 { return false }
   if int(part.get("occlusion_uv_xf0", 0)) != 0 || int(part.get("occlusion_uv_xf1", 0)) != 0 { return false }
   int(part.get("emissive_uv_xf0", 0)) == 0 && int(part.get("emissive_uv_xf1", 0)) == 0
}

fn _scene_vertex_material_batch_part_ok(any part, int first_alpha, int first_normal_word) bool {
   if int(part.get("base_color_u32", 0xffffffff)) != 0xffffffff { return false }
   if int(part.get("tex_id", -1)) < 0 { return false }
   if int(part.get("normal_tex_id", -1)) >= 0 { return false }
   if int(part.get("emissive_tex_id", -1)) >= 0 { return false }
   if int(part.get("occlusion", -1)) >= 0 { return false }
   if int(part.get("alpha_u32", 0)) != first_alpha { return false }
   if (first_alpha & 3) != 0 || band(first_alpha, 0xff000000) != 0 { return false }
   def material_u32 = int(part.get("material_u32", 0x0000ff00))
   if band(bshr(material_u32, 16), 0x7fff) != 0 { return false }
   if int(part.get("ext2_tex_word", 0x80000000)) != 0x80000000 { return false }
   if !_scene_part_uv_xforms_zero(part) { return false }
   if int(part.get("vc_mode", 0)) != 0 { return false }
   def normal_word = int(part.get("normal_tex_word", first_normal_word))
   band(normal_word, 0xffff) >= MAX_TEXTURES
}

fn _scene_vertex_material_batch_candidate(any parts, f64 scene_area_like) bool {
   if !_scene_pack_upload_candidate(parts) { return false }
   def first = parts.get(0, 0)
   if !is_dict(first) { return false }
   def first_opts = _scene_part_opts(first, scene_area_like)
   def first_flags = _scene_gpu_flags_from_part(first, first_opts)
   def first_alpha = int(first.get("alpha_u32", 0))
   def first_normal_word = int(first.get("normal_tex_word", render_utils.pack_normal_tex_word(-1, 0, 1.0, false)))
   def first_flip = _scene_part_opt_bit(first, first_opts, "flip_winding")
   def first_double = _scene_part_opt_bit(first, first_opts, "double_sided")
   mut i = 0
   def parts_n = parts.len
   while i < parts_n {
      def part = parts.get(i, 0)
      if !is_dict(part) { return false }
      def opts = _scene_part_opts(part, scene_area_like)
      if _scene_gpu_flags_from_part(part, opts)!= first_flags { return false }
      def flip = _scene_part_opt_bit(part, opts, "flip_winding")
      def dbl = _scene_part_opt_bit(part, opts, "double_sided")
      if flip != first_flip || dbl != first_double { return false }
      if !_scene_vertex_material_batch_part_ok(part, first_alpha, first_normal_word) { return false }
      if !_scene_part_bsdf_same_as(part, first) { return false }
      i += 1
   }
   true
}

fn _scene_parts_vertex_index_counts(list parts) list {
   def parts_n = parts.len
   mut total_v, total_i = 0, 0
   mut i = 0
   while i < parts_n {
      def part = parts.get(i, 0)
      total_v += int(part.get("vcnt", 0))
      total_i += int(part.get("icnt", 0))
      i += 1
   }
   [parts_n, total_v, total_i]
}

fn _upload_scene_gpu_parts_vertex_material_batch(list parts, f64 scene_area_like) any {
   if !_scene_vertex_material_batch_candidate(parts, scene_area_like) { return 0 }
   def counts = _scene_parts_vertex_index_counts(parts)
   def parts_n = counts[0]
   def total_v = counts[1]
   def total_i = counts[2]
   def vout, iout = malloc(total_v * VERTEX_STRIDE), malloc(total_i * 4)
   if !vout || !iout { return _scene_free_two(vout, iout) }
   mut vbase = 0
   mut iwrite = 0
   mut bounds_acc = _scene_bounds_accum_new()
   mut source_resources = list(0)
   mut i = 0
   while i < parts_n {
      def part = parts.get(i, 0)
      def vptr = part.get("vptr", 0)
      def iptr = part.get("iptr", 0)
      def vcnt = int(part.get("vcnt", 0))
      def icnt = int(part.get("icnt", 0))
      if !vptr || !iptr || vcnt <= 0 || icnt <= 0 { return _scene_free_two(vout, iout) }
      def vdst = vout + vbase * VERTEX_STRIDE
      memcpy(vdst, vptr, vcnt * VERTEX_STRIDE)
      if !_bake_vertex_buffer_model(vdst, vcnt, _as_render_model_mat4(part.get("model", 0))) { return _scene_free_two(vout, iout) }
      def tex_id = int(part.get("tex_id", -1))
      def material_u32 = int(part.get("material_u32", 0x0000ff00))
      _scene_store_vertex_attrs(vdst, vcnt, tex_id, material_u32, true)
      def idx_u32 = _scene_part_index_u32(part)
      if !_scene_copy_indices_u32(iout, iwrite, iptr, icnt, vcnt, vbase, idx_u32, true) { return _scene_free_two(vout, iout) }
      iwrite += icnt
      def bounds = _scene_bounds_from_vertex_ptr(vdst, vcnt)
      bounds_acc = _scene_bounds_accum_part(bounds_acc, part, bounds)
      source_resources = source_resources.append(_scene_source_part_cpu_resource(part))
      vbase += vcnt
      i += 1
   }
   mut gpu_opts = dict(4)
   gpu_opts["storage"] = "static"
   gpu_opts["index_type_u32"] = true
   def gpu_part = mesh_create_indexed(vout, total_v, iout, total_i, gpu_opts)
   if !gpu_part { return _scene_free_two(vout, iout) }
   def sbuf_h = gpu_part.get("sbuf_handle", 0)
   if !sbuf_h {
      mesh_destroy(gpu_part)
      return 0
   }
   mut merged = parts.get(0, 0)
   merged["vptr"] = vout
   merged["vcnt"] = total_v
   merged["iptr"] = iout
   merged["icnt"] = total_i
   merged["tex_id"] = -1
   merged["base_color_u32"] = 0xffffffff
   merged["material_u32"] = int(merged.get("material_u32", 0x0000ff00))
   merged["model"] = _identity_model_mat4()
   merged["node_idx"] = -1
   merged["mat_idx"] = -1
   merged["instanced_part"] = false
   merged["vc_mode"] = _SCENE_VC_VERTEX_TEX | _SCENE_VC_VERTEX_MATERIAL
   def merged_bounds = _scene_bounds_accum_result(bounds_acc)
   merged["min"] = merged_bounds.get(0, [0.0, 0.0, 0.0])
   merged["max"] = merged_bounds.get(1, [0.0, 0.0, 0.0])
   merged["merged_parts"] = parts_n
   def part_opts = _scene_part_opts(merged, scene_area_like)
   def flags = _scene_gpu_flags_from_part(merged, part_opts)
   def rec = _scene_gpu_record_for_part(merged,
      gpu_part,
      sbuf_h,
      gpu_part.get("sbuf_offset",
      0),
      gpu_part.get("ibuf",
      0),
      gpu_part.get("ibuf_offset",
      0),
      total_v,
      total_i,
      flags,
   1)
   {
      "gpu_parts": [rec], "upload_ok": 1, "upload_fail": 0,
      "gpu_v_count": total_v, "gpu_i_count": total_i, "gpu_resources": source_resources.append(gpu_part),
      "gpu_optical_start": 1, "gpu_blend_start": 1,
      "has_blend": false, "has_optical": false, "vertex_material_batch": true
   }
}

fn _scene_gpu_flags_from_part(any part, any opts) int {
   def use_points = _scene_part_opt_bool(part, "is_points", false) || (is_dict(opts) && opts.get("is_points", false))
   def use_lines = !use_points && (_scene_part_opt_bool(part, "is_lines", false) || (is_dict(opts) && opts.get("is_lines", false)))
   mut flags = use_points ? _MESH_GPU_POINTS : (use_lines ? _MESH_GPU_LINES : 0)
   if _scene_part_opt_bool(part, "unlit", false)|| (is_dict(opts) && opts.get("unlit", false)) { flags = flags | _MESH_GPU_UNLIT }
   if _scene_part_opt_bool(part, "no_cull", false)|| (is_dict(opts) && opts.get("no_cull", false)) { flags = flags | _MESH_GPU_NOCULL }
   flags | _MESH_GPU_INDEXED
}

fn _scene_gpu_record_for_part(
   dict part,
   any gpu_part,
   any sbuf_h,
   int sbuf_off,
   any ibuf,
   int ibuf_off,
   int draw_cnt,
   int idx_cnt,
   int flags,
   int index_type_val
)  list {
   def tex_id = part.get("tex_id",-1)
   def part_opts = part.get("opts", 0)
   def model_m = part.get("model",0)
   def node_idx = int(part.get("node_idx",-1))
   def base_color_u32 = int(part.get("base_color_u32",0xffffffff))
   def material_u32 = int(part.get("material_u32",0x0000ff00))
   def emissive_tex_id = int(part.get("emissive_tex_id",-1))
   def emissive_u32 = int(part.get("emissive_u32",0))
   def emissive_uv_set = int(part.get("emissive_uv_set",0))
   def alpha_u32 = int(part.get("alpha_u32",0))
   def occlusion_id = int(part.get("occlusion",-1))
   def occlusion_uv_set = int(part.get("occlusion_uv_set",0))
   def bsdf0_u32 = int(part.get("bsdf0_u32",0))
   def bsdf1_u32 = int(part.get("bsdf1_u32",0))
   def bsdf2_u32 = int(part.get("bsdf2_u32",0))
   def bsdf3_u32 = int(part.get("bsdf3_u32",0))
   def bsdf4_u32 = int(part.get("bsdf4_u32",0))
   def bsdf5_u32 = int(part.get("bsdf5_u32",0))
   def normal_tex_id = int(part.get("normal_tex_id",-1))
   def flip_winding = (
      part.get("flip_winding", false)
      || (is_dict(part_opts) && part_opts.get("flip_winding", false))
   ) ? 1 : 0
   def double_sided = (
      part.get("double_sided", false)
      || (is_dict(part_opts) && part_opts.get("double_sided", false))
   ) ? 1 : 0
   mut normal_tex_word = int(part.get("normal_tex_word", render_utils.pack_normal_tex_word(normal_tex_id,
            int(part.get("normal_uv_set",
   0)))))
   if double_sided != 0 && flip_winding != 0 { normal_tex_word = bor(normal_tex_word, 0x20000) }
   if double_sided != 0 { normal_tex_word = bor(normal_tex_word, 0x40000) }
   def normal_uv_set = int(part.get("normal_uv_set",0))
   def base_uv_xf0 = int(part.get("base_uv_xf0",0))
   def base_uv_xf1 = int(part.get("base_uv_xf1",0))
   def normal_uv_xf0 = int(part.get("normal_uv_xf0",0))
   def normal_uv_xf1 = int(part.get("normal_uv_xf1",0))
   def mr_uv_xf0 = int(part.get("mr_uv_xf0",0))
   def mr_uv_xf1 = int(part.get("mr_uv_xf1",0))
   def occlusion_uv_xf0 = int(part.get("occlusion_uv_xf0",0))
   def occlusion_uv_xf1 = int(part.get("occlusion_uv_xf1",0))
   def emissive_uv_xf0 = int(part.get("emissive_uv_xf0",0))
   def emissive_uv_xf1 = int(part.get("emissive_uv_xf1",0))
   def vc_mode = int(part.get("vc_mode", 0))
   def visible_u32 = part.get("visible", true) ? 1 : 0
   def minv = part.get("min", [0.0, 0.0, 0.0])
   def maxv = part.get("max", [0.0, 0.0, 0.0])
   def mat_idx = int(part.get("mat_idx", part.get("material_idx", -1)))
   [
      tex_id, 1.0, sbuf_h, sbuf_off, ibuf, ibuf_off, draw_cnt, idx_cnt, flags,
      index_type_val,
      model_m,
      node_idx,
      base_color_u32,
      material_u32,
      emissive_tex_id,
      emissive_u32,
      emissive_uv_set,
      alpha_u32,
      occlusion_id,
      occlusion_uv_set,
      bsdf0_u32, bsdf1_u32,
      bsdf2_u32, bsdf3_u32,
      normal_tex_word,
      normal_uv_set,
      base_uv_xf0, base_uv_xf1,
      normal_uv_xf0, normal_uv_xf1,
      mr_uv_xf0, mr_uv_xf1,
      occlusion_uv_xf0, occlusion_uv_xf1,
      emissive_uv_xf0, emissive_uv_xf1,
      part.get("instanced_part", false) ? 1 : 0,
      visible_u32,
      minv,
      maxv,
      gpu_part,
      bsdf4_u32,
      bsdf5_u32,
      int(part.get("ext2_tex_word", 0x80000000)),
      flip_winding,
      vc_mode,
      double_sided,
      mat_idx
   ]
}

fn _upload_scene_gpu_parts_packed(any parts, f64 scene_area_like) any {
   if !_scene_pack_upload_candidate(parts) { return 0 }
   def counts = _scene_parts_vertex_index_counts(parts)
   def parts_n = counts[0]
   def total_v = counts[1]
   def total_i = counts[2]
   def vout, iout = malloc(total_v * VERTEX_STRIDE), malloc(total_i * 4)
   if !vout || !iout { return _scene_free_two(vout, iout) }
   mut vbase = 0
   mut iwrite = 0
   mut source_resources = list(0)
   mut i = 0
   while i < parts_n {
      def part = parts.get(i, 0)
      def vptr = part.get("vptr", 0)
      def iptr = part.get("iptr", 0)
      def vcnt = int(part.get("vcnt", 0))
      def icnt = int(part.get("icnt", 0))
      def tex_id = int(part.get("tex_id", -1))
      memcpy(vout + vbase * VERTEX_STRIDE, vptr, vcnt * VERTEX_STRIDE)
      _scene_store_vertex_attrs(vout + vbase * VERTEX_STRIDE, vcnt, tex_id)
      def idx_u32 = _scene_part_index_u32(part)
      _scene_copy_indices_u32(iout, iwrite, iptr, icnt, vcnt, vbase, idx_u32)
      source_resources = source_resources.append(_scene_source_part_cpu_resource(part))
      vbase += vcnt
      iwrite += icnt
      i += 1
   }
   mut gpu_opts = dict(4)
   gpu_opts["storage"] = "static"
   gpu_opts["index_type_u32"] = true
   def gpu_part = mesh_create_indexed(vout, total_v, iout, total_i, gpu_opts)
   if !gpu_part { return _scene_free_two(vout, iout) }
   def sbuf_h = gpu_part.get("sbuf_handle",0)
   if !sbuf_h {
      mesh_destroy(gpu_part)
      return 0
   }
   def sbuf_off = gpu_part.get("sbuf_offset",0)
   def ibuf = gpu_part.get("ibuf",0)
   def ibuf_off = gpu_part.get("ibuf_offset",0)
   mut gpu_parts = list(0)
   mut has_blend = false
   mut has_optical = false
   i = 0
   iwrite = 0
   while i < parts_n {
      def part = parts.get(i, 0)
      def icnt = int(part.get("icnt", 0))
      def vcnt = int(part.get("vcnt", 0))
      def part_opts = _scene_part_opts(part, scene_area_like)
      def flags = _scene_gpu_flags_from_part(part, part_opts)
      def alpha_u32 = int(part.get("alpha_u32",0))
      def rec = _scene_gpu_record_for_part(part,
         gpu_part,
         sbuf_h,
         sbuf_off,
         ibuf,
         ibuf_off + iwrite * 4,
         vcnt,
         icnt,
         flags,
      1)
      gpu_parts = gpu_parts.append(rec)
      if _gpu_rec_is_optical(rec) { has_optical = true }
      if (alpha_u32 & 3)== 2 { has_blend = true }
      iwrite += icnt
      i += 1
   }
   if parts_n > 1 && (has_blend || has_optical || parts_n < 512) { _sort_gpu_parts_by_blend_tex(gpu_parts) }
   def gpu_optical_start = _gpu_parts_optical_start(gpu_parts)
   def gpu_blend_start = _gpu_parts_blend_start(gpu_parts)
   {
      "gpu_parts": gpu_parts, "upload_ok": gpu_parts.len, "upload_fail": 0,
      "gpu_v_count": total_v, "gpu_i_count": total_i, "gpu_resources": source_resources.append(gpu_part),
      "gpu_optical_start": gpu_optical_start, "gpu_blend_start": gpu_blend_start,
      "has_blend": has_blend, "has_optical": has_optical, "packed_upload": true
   }
}

fn _upload_scene_gpu_parts(any parts) dict {
   mut gpu_parts = list(0)
   mut gpu_resources = list(0)
   mut upload_ok = 0
   mut upload_fail = 0
   mut gpu_v_count = 0
   mut gpu_i_count = 0
   mut has_blend = false
   mut has_optical = false
   def scene_area_like = _scene_parts_area_like(parts)
   if is_list(parts) && parts.len >= 4096 && common.env_enabled("NY_UI_VERTEX_MATERIAL_BATCH") {
      def vertex_batch = _upload_scene_gpu_parts_vertex_material_batch(parts, scene_area_like)
      if is_dict(vertex_batch)&& int(vertex_batch.get("upload_ok", 0)) > 0 { return vertex_batch }
   }
   if is_list(parts) && parts.len >= 4096 && !common.env_enabled("NY_UI_FORCE_MERGE_BEFORE_PACKED_UPLOAD") {
      def packed_first = _upload_scene_gpu_parts_packed(parts, scene_area_like)
      if is_dict(packed_first)&& int(packed_first.get("upload_ok", 0)) > 0 { return packed_first }
   }
   def merge_state = _merge_scene_static_indexed_parts(parts, scene_area_like)
   if is_dict(merge_state) {
      parts, gpu_resources = merge_state.get("parts", parts), merge_state.get("source_resources", gpu_resources)
      if (_scene_group_trace_enabled()
      || _scene_force_group_diag_enabled())
      && int(merge_state.get("merge_count", 0)) > 0{
         ui_profile.print_text("[group:merge] parts=" + to_str(parts.len) + " removed=" + to_str(int(merge_state.get("merge_count",
         0))))
      }
   }
   def packed_upload = _upload_scene_gpu_parts_packed(parts, scene_area_like)
   if is_dict(packed_upload)&& int(packed_upload.get("upload_ok", 0)) > 0 { return packed_upload }
   if common.env_enabled("NY_GLTF_GROUPED_GPU_DIRECT_OFF") {
      return {
         "gpu_parts": [], "upload_ok": 0, "upload_fail": is_list(parts) ? parts.len : 0,
         "gpu_optical_start": 0, "gpu_blend_start": 0,
         "gpu_v_count": 0, "gpu_i_count": 0, "gpu_resources": gpu_resources,
         "has_blend": false, "has_optical": false
      }
   }
   def part_count = parts.len
   mut pi = 0
   while pi < part_count {
      def part = parts.get(pi)
      def vptr = part.get("vptr",0)
      def vcnt = part.get("vcnt",0)
      def iptr = part.get("iptr",0)
      def icnt = part.get("icnt",0)
      def tex_id = part.get("tex_id",-1)
      def part_opts = part.get("opts", 0)
      if vptr && vcnt > 0 {
         _scene_store_vertex_attrs(vptr, int(vcnt), int(tex_id))
      }
      mut gpu_opts = _scene_part_opts(part, scene_area_like, part_opts)
      gpu_opts["storage"] = "static"
      mut gpu_part = 0
      if iptr && icnt > 0 {
         gpu_part = mesh_create_indexed(vptr, vcnt, iptr, icnt, gpu_opts)
      } else {
         gpu_part = mesh_create_static(vptr, vcnt, false, gpu_opts)
      }
      if !gpu_part {
         if _scene_force_group_diag_enabled() {
            print(
               "[group:upload] mesh_create_failed part=" + to_str(pi)
               + " vcnt=" + to_str(vcnt)
               + " icnt=" + to_str(icnt)
               + " opts_dict=" + to_str(is_dict(gpu_opts))
               + " storage=" + to_str(is_dict(gpu_opts) ? gpu_opts.get("storage", "?") : "?")
            )
         }
         upload_fail += 1 pi += 1 continue
      }
      def sbuf_h    = gpu_part.get("sbuf_handle",0)
      if !sbuf_h {
         if _scene_force_group_diag_enabled() {
            print(
               "[group:upload] missing_sbuf_handle part=" + to_str(pi)
               + " type=" + type(gpu_part)
               + " handle=" + to_str(gpu_part.get("handle", 0))
               + " sbuf=" + to_str(gpu_part.get("sbuf", 0))
               + " keys=" + to_str(dict_keys(gpu_part))
            )
         }
         upload_fail += 1 pi += 1 continue
      }
      gpu_resources = gpu_resources.append(gpu_part)
      def sbuf_off  = gpu_part.get("sbuf_offset",0)
      def ibuf      = gpu_part.get("ibuf",0)
      def ibuf_off  = gpu_part.get("ibuf_offset",0)
      def draw_cnt  = gpu_part.get("draw_count",vcnt)
      def idx_cnt   = gpu_part.get("draw_index_count",icnt)
      def flags     = gpu_part.get("render_flags",0)
      def index_type_val = gpu_part.get("index_type_u32",false) ? 1 : 0
      def gpu_rec = _scene_gpu_record_for_part(part,
         gpu_part,
         sbuf_h,
         sbuf_off,
         ibuf,
         ibuf_off,
         draw_cnt,
         idx_cnt,
         flags,
      index_type_val)
      if (_scene_group_trace_enabled() || _scene_force_group_diag_enabled()) && gpu_parts.len < 6 {
         ui_profile.print_text("[group:upload] part=" + to_str(pi) +
            " sbuf=" + to_str(sbuf_h) +
            " ibuf=" + to_str(ibuf) +
            " soff=" + to_str(sbuf_off) +
            " ioff=" + to_str(ibuf_off) +
            " draw=" + to_str(draw_cnt) +
            " idx=" + to_str(idx_cnt) +
            " flags=0x" + str.to_hex(flags) +
         " idx32=" + to_str(index_type_val))
      }
      gpu_parts = gpu_parts.append(gpu_rec)
      if _gpu_rec_is_optical(gpu_rec) { has_optical = true }
      if (_scene_list_int_safe(gpu_rec, 17, 0) & 3) == 2 { has_blend = true }
      upload_ok += 1 gpu_v_count += vcnt gpu_i_count += icnt
      pi += 1
   }
   if upload_ok > 1 && (has_blend || has_optical || upload_ok < 512) { _sort_gpu_parts_by_blend_tex(gpu_parts) }
   def gpu_optical_start = _gpu_parts_optical_start(gpu_parts)
   def gpu_blend_start = _gpu_parts_blend_start(gpu_parts)
   {
      "gpu_parts": gpu_parts, "upload_ok": upload_ok, "upload_fail": upload_fail,
      "gpu_optical_start": gpu_optical_start, "gpu_blend_start": gpu_blend_start,
      "gpu_v_count": gpu_v_count, "gpu_i_count": gpu_i_count, "gpu_resources": gpu_resources,
      "has_blend": has_blend, "has_optical": has_optical
   }
}

fn _build_scene_render_parts(any gpu_parts) list {
   "Builds a lightweight render-only part list from grouped GPU records."
   if !is_list(gpu_parts)|| gpu_parts.len == 0 { return [] }
   def gpu_n = gpu_parts.len
   mut out = []
   mut i = 0
   while i < gpu_n {
      def rec = gpu_parts.get(i, 0)
      if is_list(rec) && rec.len >= 45 {
         def mesh = _scene_list_any_safe(rec, 40, 0)
         if is_dict(mesh) {
            def model_m = _scene_list_any_safe(rec, 10, 0)
            mut model = render._model_matrix_to_render_mat(model_m)
            if !is_list(model)|| model.len != 18 { model = _identity_model_mat4() }
            def flags = _scene_list_int_safe(rec, 8, 0)
            mut mesh2 = mesh
            mesh2["tex_id"] = _scene_list_int_safe(rec, 0, -1)
            def part_flip_winding = _scene_list_int_safe(rec, 44, 0) != 0
            def part_vc_mode = _scene_list_int_safe(rec, 45, 0)
            def part_double_sided = _scene_list_int_safe(rec, 46, 0) != 0
            mesh2["vc_mode"] = part_vc_mode
            mesh2["flip_winding"] = part_flip_winding
            mesh2["double_sided"] = part_double_sided
            mut part = dict(10)
            part["mesh"] = mesh2
            part["mat_idx"] = int(mesh.get("mat_idx", mesh.get("material_idx", -1)))
            part["model"] = model
            part["visible"] = _scene_list_int_safe(rec, 37, 1) != 0
            part["vc_mode"] = part_vc_mode
            part["flip_winding"] = part_flip_winding
            part["double_sided"] = part_double_sided
            mut ri = 0
            def render_rec_i32_fields_n = _SCENE_RENDER_REC_I32_FIELDS.len
            while ri < render_rec_i32_fields_n {
               def f = _SCENE_RENDER_REC_I32_FIELDS.get(ri, [])
               part[f.get(0, "")] = _scene_list_int_safe(rec, int(f.get(1, 0)), int(f.get(2, 0)))
               ri += 1
            }
            part["is_lines"] = band(flags, _MESH_GPU_LINES) != 0
            part["is_points"] = band(flags, _MESH_GPU_POINTS) != 0
            part["unlit"] = band(flags, _MESH_GPU_UNLIT) != 0
            part["width"] = _scene_list_num_safe(rec, 1, 1.0)
            part["min"] = _scene_list_any_safe(rec, 38, [0.0, 0.0, 0.0])
            part["max"] = _scene_list_any_safe(rec, 39, [0.0, 0.0, 0.0])
            def part_mat_slab = render_utils.pack_material_slab(part)
            if part_mat_slab {
               mesh2["material_slab"] = part_mat_slab
               part["material_slab"] = part_mat_slab
            }
            part["mesh"] = mesh2
            out = out.append(part)
         }
      }
      i += 1
   }
   _sort_cpu_parts_by_blend_tex(out)
   out
}

fn _scene_attach_mesh_metadata(any mesh_in, any part_in, bool include_bounds=false) list {
   mut mesh = mesh_in
   mut part = part_in
   if !is_dict(mesh)|| !is_dict(part) { return [mesh, part] }
   mesh["tex_id"] = int(part.get("tex_id", -1))
   mesh["vc_mode"] = int(part.get("vc_mode", 0))
   def opts = part.get("opts", 0)
   mesh["flip_winding"] = part.get("flip_winding", false) || (is_dict(opts) && opts.get("flip_winding", false))
   mesh["double_sided"] = part.get("double_sided", false) || (is_dict(opts) && opts.get("double_sided", false))
   def part_mat_slab = render_utils.pack_material_slab(part)
   if part_mat_slab {
      mesh["material_slab"] = part_mat_slab
      part["material_slab"] = part_mat_slab
   }
   if include_bounds {
      if part.contains("min") { mesh["min"] = part.get("min", [0.0, 0.0, 0.0]) }
      if part.contains("max") { mesh["max"] = part.get("max", [0.0, 0.0, 0.0]) }
   }
   [mesh, part]
}

fn _scene_finalize_cpu_mesh_part(any mesh, any part, bool baked_ok=false) list {
   if mesh == 0 { return [mesh, part] }
   def tagged = _scene_attach_mesh_metadata(mesh, part, true)
   mesh, part = tagged.get(0, mesh), tagged.get(1, part)
   part["mesh"] = mesh
   if baked_ok { part["model"] = _identity_model_mat4() }
   [mesh, part]
}

fn _scene_free_skin_merge_buffers(any bind_vptr, any joints_ptr, any weights_ptr, any idx_ptr) int {
   if bind_vptr { free(bind_vptr) }
   if joints_ptr { free(joints_ptr) }
   if weights_ptr { free(weights_ptr) }
   if idx_ptr { free(idx_ptr) }
   0
}

fn _scene_skinned_solid_merge_candidate(any part, f64 scene_area_like) bool {
   if !is_dict(part) { return false }
   if _cpu_part_pass_bucket(part)!= 0 { return false }
   if int(part.get("skin_idx", -1)) < 0 { return false }
   if _scene_part_bool_field(part, "instanced_part", false) { return false }
   if _scene_part_is_backdrop(part, scene_area_like) { return false }
   if part.get("primitive_mode", 4)!= 4 { return false }
   def opts = part.get("opts", 0)
   if is_dict(opts)&& (opts.get("is_lines", false) || opts.get("is_points", false)) { return false }
   if int(part.get("vc_mode", 0)) != 0 { return false }
   if int(part.get("tex_id", -1)) >= 0 { return false }
   if int(part.get("normal_tex_id", -1)) >= 0 { return false }
   if int(part.get("emissive_tex_id", -1)) >= 0 { return false }
   if int(part.get("occlusion", -1)) >= 0 { return false }
   if int(part.get("ext2_tex_word", 0x80000000)) != 0x80000000 { return false }
   if !part.get("vptr", 0)|| !part.get("iptr", 0) { return false }
   if !part.get("skin_bind_vptr", 0)|| !part.get("skin_joints_ptr", 0) || !part.get("skin_weights_ptr", 0) { return false }
   _scene_part_indices_valid(part)
}

fn _scene_skinned_solid_merge_compatible(any first, any next, int total_vcnt, f64 scene_area_like) bool {
   if !_scene_skinned_solid_merge_candidate(first, scene_area_like) { return false }
   if !_scene_skinned_solid_merge_candidate(next, scene_area_like) { return false }
   if total_vcnt <= 0 { return false }
   if int(first.get("skin_idx", -1)) != int(next.get("skin_idx", -1)) { return false }
   if int(first.get("skin_inv_bind_accessor", -1)) != int(next.get("skin_inv_bind_accessor", -1)) { return false }
   if int(first.get("skin_joint_count", 0)) != int(next.get("skin_joint_count", 0)) { return false }
   if !_scene_part_same_model(first, next) { return false }
   if !_scene_parts_same_i32_fields(first, next, _scene_skin_solid_merge_i32_field_defaults()) { return false }
   if !_scene_part_same_i32(first, next, "material_u32", 0x0000ff00) { return false }
   if !_scene_parts_same_normal_word(first, next) { return false }
   if _scene_part_bool_field(first, "visible", true)!= _scene_part_bool_field(next, "visible", true) { return false }
   _scene_parts_same_merge_bool_fields(first, next)
}

fn _scene_merge_skinned_solid_run(list parts, int start_idx, int end_idx, int total_vcnt, int total_icnt) any {
   if end_idx <= start_idx + 1 || total_vcnt <= 0 || total_icnt <= 0 { return 0 }
   def bind_out = malloc(total_vcnt * VERTEX_STRIDE)
   def joints_out = malloc(total_vcnt * 16)
   def weights_out = malloc(total_vcnt * 16)
   def iout = malloc(total_icnt * 4)
   if !bind_out || !joints_out || !weights_out || !iout { return _scene_free_skin_merge_buffers(bind_out, joints_out, weights_out, iout) }
   mut vbase = 0
   mut iwrite = 0
   mut bounds_acc = _scene_bounds_accum_new()
   mut single_influence = true
   mut pi = start_idx
   while pi < end_idx {
      def part = parts.get(pi, 0)
      def bind_src = part.get("skin_bind_vptr", part.get("vptr", 0))
      def joints_src = part.get("skin_joints_ptr", 0)
      def weights_src = part.get("skin_weights_ptr", 0)
      def iptr = part.get("iptr", 0)
      def vcnt = int(part.get("vcnt", 0))
      def icnt = int(part.get("icnt", 0))
      if !bind_src || !joints_src || !weights_src || !iptr || vcnt <= 0 || icnt <= 0 { return _scene_free_skin_merge_buffers(bind_out, joints_out, weights_out, iout) }
      def color_u32 = int(part.get("base_color_u32", 0xffffffff))
      memcpy(bind_out + vbase * VERTEX_STRIDE, bind_src, vcnt * VERTEX_STRIDE)
      memcpy(joints_out + vbase * 16, joints_src, vcnt * 16)
      memcpy(weights_out + vbase * 16, weights_src, vcnt * 16)
      mut viw = 0
      while viw < vcnt {
         def vdst = bind_out + (vbase + viw) * VERTEX_STRIDE
         store32(vdst, color_u32, _VKR_OFF_C)
         store32(vdst, -1, _VKR_OFF_TEX)
         if single_influence {
            def side = viw * 16
            def w0 = float(load32_f32(weights_src, side + 0))
            def w1 = float(load32_f32(weights_src, side + 4))
            def w2 = float(load32_f32(weights_src, side + 8))
            def w3 = float(load32_f32(weights_src, side + 12))
            def j0 = int(load32(joints_src, side + 0))
            if !(j0 >= 0 && w0 >= 0.999999 && abs(w1)<= 0.000001 && abs(w2) <= 0.000001 && abs(w3) <= 0.000001) { single_influence = false }
         }
         viw += 1
      }
      def idx_u32 = _scene_part_index_u32(part)
      if !_scene_copy_indices_u32(iout, iwrite, iptr, icnt, vcnt, vbase, idx_u32, true) { return _scene_free_skin_merge_buffers(bind_out, joints_out, weights_out, iout) }
      iwrite += icnt
      bounds_acc = _scene_bounds_accum_part(bounds_acc, part)
      vbase += vcnt
      pi += 1
   }
   def merged_bounds = _scene_bounds_accum_result(bounds_acc)
   mut merged = parts.get(start_idx, 0)
   mut opts = merged.get("opts", 0)
   if !is_dict(opts) { opts = dict(4) }
   opts["index_type_u32"] = true
   opts["vc_mode"] = 1
   merged["vptr"] = bind_out
   merged["vcnt"] = total_vcnt
   merged["iptr"] = iout
   merged["icnt"] = total_icnt
   merged["opts"] = opts
   merged["skin_bind_vptr"] = bind_out
   merged["skin_joints_ptr"] = joints_out
   merged["skin_weights_ptr"] = weights_out
   merged["skin_vcnt"] = total_vcnt
   merged["index_type_u32"] = true
   merged["vc_mode"] = 1
   merged["base_color_u32"] = 0xffffffff
   merged["tex_id"] = -1
   merged["mat_idx"] = -1
   merged["min"] = merged_bounds.get(0, [0.0, 0.0, 0.0])
   merged["max"] = merged_bounds.get(1, [0.0, 0.0, 0.0])
   merged["merged_parts"] = end_idx - start_idx
   merged["skinned_solid_merged"] = true
   if single_influence { merged["skin_single_influence"] = true }
   merged
}

fn _merge_scene_skinned_solid_parts(any parts, f64 scene_area_like) any {
   if !is_list(parts)|| parts.len <= 1 { return 0 }
   mut out = list(0)
   mut merge_count = 0
   def parts_n = parts.len
   mut i = 0
   while i < parts_n {
      def first = parts.get(i, 0)
      mut run_end = i + 1
      mut total_vcnt = int(first.get("vcnt", 0))
      mut total_icnt = int(first.get("icnt", 0))
      if _scene_skinned_solid_merge_candidate(first, scene_area_like) {
         while run_end < parts_n {
            def next = parts.get(run_end, 0)
            def next_vcnt = int(next.get("vcnt", 0))
            def next_icnt = int(next.get("icnt", 0))
            if !_scene_skinned_solid_merge_compatible(first, next, total_vcnt + next_vcnt, scene_area_like) { break }
            total_vcnt += next_vcnt
            total_icnt += next_icnt
            run_end += 1
         }
      }
      if run_end > i + 1 {
         def merged = _scene_merge_skinned_solid_run(parts, i, run_end, total_vcnt, total_icnt)
         if is_dict(merged) {
            out = out.append(merged)
            merge_count += (run_end - i - 1)
            i = run_end
            continue
         }
      }
      out = out.append(first)
      i += 1
   }
   if merge_count <= 0 { return 0 }
   mut r = dict(2)
   r["parts"] = out
   r["merge_count"] = merge_count
   r
}

fn _build_scene_cpu_parts(any parts, bool fallback_unlit=false) list {
   "Builds per-part meshes for grouped scene fallback drawing.
   Keeps original CPU vertex/index data alive so grouped fallback drawing uses
   the exact source primitive topology and avoids unstable grouped GPU
   static-draw path."
   def diag_on = _scene_diag_enabled()
   mut out = []
   mut total_v = 0
   mut total_i = 0
   mut built_meshes = 0
   mut failed_meshes = 0
   mut expanded_indexed = 0
   mut expanded_failed = 0
   mut has_blend = false
   def can_dynamic_indexed = render.renderer_gpu_ready()
   def scene_area_like = _scene_parts_area_like(parts)
   def group_trace = _scene_group_trace_enabled()
   if group_trace { terminal.log("[cpu_parts] begin parts=" + to_str(is_list(parts) ? parts.len : -1) + " area=" + to_str(scene_area_like)) }
   if can_dynamic_indexed {
      def skin_merge = _merge_scene_skinned_solid_parts(parts, scene_area_like)
      if is_dict(skin_merge) {
         parts = skin_merge.get("parts", parts)
         if diag_on || _scene_group_trace_enabled() {
            terminal.log("[cpu_parts:skin_merge] parts=" + to_str(parts.len) + " removed=" + to_str(int(skin_merge.get("merge_count",
            0))))
         }
      }
   }
   def part_count = parts.len
   def bake_cpu_model = false && common.env_enabled("NY_GLTF_CPU_BAKE_MODEL")
   mut pi = 0
   while pi < part_count {
      mut part = parts.get(pi, 0)
      if is_dict(part) {
         if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " begin") }
         def src_vptr = part.get("vptr", 0)
         def vcnt = int(part.get("vcnt", 0))
         def iptr = part.get("iptr", 0)
         def icnt = int(part.get("icnt", 0))
         def tex_id = int(part.get("tex_id", -1))
         def skinned_indexed = can_dynamic_indexed && int(part.get("skin_idx", -1)) >= 0 && iptr && icnt > 0
         mut vptr = 0
         if src_vptr && vcnt > 0 {
            def live_vptr = _clone_vertices(src_vptr, vcnt)
            if live_vptr {
               vptr = live_vptr
               part["vptr"] = live_vptr
            }
         }
         if (int(part.get("alpha_u32", 0)) & 3) == 2 { has_blend = true }
         def model_m = part.get("model", 0)
         if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " opts") }
         mut opts = _scene_part_opts(part, scene_area_like)
         if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " opts_ok storage=" + to_str(is_dict(opts) ? opts.get("storage", "") : "?")) }
         if fallback_unlit {
            opts["unlit"] = true
            opts["no_cull"] = true
            part["unlit"] = true
            part["no_cull"] = true
         }
         def is_lines = opts.get("is_lines", false)
         if vptr && vcnt > 0 {
            _scene_store_vertex_attrs(vptr, vcnt, tex_id)
            if diag_on && pi == 0 {
               def c0, t0 = load32(vptr, _VKR_OFF_C), load32(vptr, _VKR_OFF_TEX)
               def x0, y0 = load32_f32(vptr, _VKR_OFF_X), load32_f32(vptr, _VKR_OFF_Y)
               def z0 = load32_f32(vptr, _VKR_OFF_Z)
               terminal.log("[cpu_parts:v0] pos=(" + to_str(x0) + "," + to_str(y0) + "," + to_str(z0) + ") color=0x" + str.to_hex(c0) + " tex=" + to_str(t0))
            }
            total_v += vcnt
         }
         if iptr && icnt > 0 { total_i += icnt }
         if vptr && vcnt > 0 && iptr && icnt > 0 {
            opts["storage"] = "cpu"
            mut mesh = 0
            mut baked_ok = false
            if bake_cpu_model {
               def idx_u32 = opts.get("index_type_u32", false)
               def exp_ptr = render_utils.gltf_expand_indexed_vertices(vptr, vcnt, iptr, icnt, idx_u32)
               if exp_ptr { expanded_indexed += 1 } else { expanded_failed += 1 }
               baked_ok = (exp_ptr != 0) ? _bake_vertex_buffer_model(exp_ptr, icnt, model_m) : false
               mesh = exp_ptr ? mesh_create_cpu(exp_ptr, icnt, is_lines, opts) : 0
               if mesh == 0 && exp_ptr { free(exp_ptr) }
            } else {
               if skinned_indexed {
                  if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " mesh_indexed_cpu") }
                  mesh = mesh_create_indexed(vptr, vcnt, iptr, icnt, opts, is_lines)
               } else {
                  if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " expand_indexed") }
                  def idx_u32 = opts.get("index_type_u32", false)
                  def exp_ptr = render_utils.gltf_expand_indexed_vertices(vptr, vcnt, iptr, icnt, idx_u32)
                  if exp_ptr { expanded_indexed += 1 } else { expanded_failed += 1 }
                  if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " mesh_cpu exp=" + to_str(bool(exp_ptr)) + " icnt=" + to_str(icnt)) }
                  mesh = exp_ptr ? mesh_create_cpu(exp_ptr, icnt, is_lines, opts) : 0
                  if mesh == 0 && exp_ptr { free(exp_ptr) }
               }
            }
            if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " mesh_done ok=" + to_str(mesh != 0)) }
            if mesh != 0 {
               if skinned_indexed { mesh["dynamic_vertices"] = true }
               built_meshes += 1
               if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " finalize") }
               def finalized = _scene_finalize_cpu_mesh_part(mesh, part, baked_ok)
               mesh, part = finalized.get(0, mesh), finalized.get(1, part)
               if group_trace { terminal.log("[cpu_parts] part=" + to_str(pi) + " finalize_ok") }
            } else {
               failed_meshes += 1
            }
         } elif vptr && vcnt > 0 {
            opts["storage"] = "cpu"
            def lin_ptr = _clone_vertices(vptr, vcnt)
            def baked_ok = (bake_cpu_model && lin_ptr) ? _bake_vertex_buffer_model(lin_ptr, vcnt, model_m) : false
            mut mesh = lin_ptr ? mesh_create_cpu(lin_ptr, vcnt, is_lines, opts) : 0
            if mesh != 0 {
               built_meshes += 1
               def finalized = _scene_finalize_cpu_mesh_part(mesh, part, baked_ok)
               mesh, part = finalized.get(0, mesh), finalized.get(1, part)
            } else {
               if lin_ptr { free(lin_ptr) }
               failed_meshes += 1
            }
         }
      }
      out = out.append(part)
      pi += 1
   }
   if group_trace { terminal.log("[cpu_parts] sort begin out=" + to_str(out.len)) }
   _sort_cpu_parts_by_blend_tex(out)
   if group_trace { terminal.log("[cpu_parts] done out=" + to_str(out.len) + " built=" + to_str(built_meshes) + " failed=" + to_str(failed_meshes)) }
   if diag_on { terminal.log("[cpu_parts] src=" + to_str(parts.len) + " built=" + to_str(built_meshes) + " failed=" + to_str(failed_meshes) + " v=" + to_str(total_v) + " i=" + to_str(total_i)) }
   [out, total_v, total_i, has_blend]
}

fn _scene_rebuild_drawable_parts_from_raw(any parts) list {
   "Converts raw glTF parts into draw-ready CPU mesh parts without mutating the source vertex buffer."
   mut out = []
   if !is_list(parts) { return out }
   def parts_n = parts.len
   def scene_area_like = _scene_parts_area_like(parts)
   mut i = 0
   while i < parts_n {
      mut part = parts.get(i, 0)
      if is_dict(part) {
         mut opts = _scene_part_opts(part, scene_area_like)
         def built = mesh_create_cpu_part_from_raw(part, opts)
         part = built.get(0, part)
         mut mesh = built.get(1, 0)
         if mesh != 0 {
            def tagged = _scene_attach_mesh_metadata(mesh, part, false)
            mesh, part = tagged.get(0, mesh), tagged.get(1, part)
            part["mesh"] = mesh
         }
      }
      out = out.append(part)
      i += 1
   }
   _sort_cpu_parts_by_blend_tex(out)
   out
}

fn _scene_tag_parts_scene_name(any src_parts, str scene_name) any {
   if !is_list(src_parts) { return src_parts }
   mut tagged = []
   mut scene_part_i = 0
   def src_n = src_parts.len
   while scene_part_i < src_n {
      def scene_part = src_parts.get(scene_part_i, 0)
      if is_dict(scene_part) {
         scene_part["scene_name"] = scene_name
         tagged = tagged.append(scene_part)
      } else {
         tagged = tagged.append(scene_part)
      }
      scene_part_i += 1
   }
   tagged
}

fn _scene_grouped_upload_stage(any src_parts, bool deform_runtime_active, bool diag_on, bool stage_trace, bool prof_on) dict {
   def have_gpu_device = render.renderer_gpu_ready()
   def grouped_gpu_disabled = common.env_enabled("NY_GLTF_GROUPED_GPU_OFF")
   || common.env_enabled("NY_GLTF_DISABLE_GROUPED_GPU")
   def grouped_gpu_requested = !grouped_gpu_disabled
   def force_cpu_group = !have_gpu_device
   || deform_runtime_active
   || common.env_enabled("NY_GLTF_FORCE_CPU_GROUPED")
   || common.env_enabled("NY_GLTF_SAFE_VISIBLE")
   || !grouped_gpu_requested
   if force_cpu_group && diag_on { terminal.log("[gltf] grouped GPU upload forced OFF") }
   def t_upload0 = ticks()
   def upload = force_cpu_group ? dict(0) : _upload_scene_gpu_parts(src_parts)
   def t_upload_ms = ui_profile.elapsed_ms(t_upload0)
   _scene_stage_ms(stage_trace, "mesh.after_upload", t_upload_ms, " gpu=" + to_str(int(upload.get("upload_ok", 0)) > 0))
   _scene_prof_ms(prof_on, "upload", t_upload_ms)
   def upload_ok = int(upload.get("upload_ok", 0))
   def upload_fail = int(upload.get("upload_fail", 0))
   def gpu_upload_usable = !force_cpu_group && upload_ok > 0
   mut upload_gpu_parts = upload.get("gpu_parts", [])
   if !is_list(upload_gpu_parts) { upload_gpu_parts = [] }
   mut upload_gpu_resources = upload.get("gpu_resources", [])
   if !is_list(upload_gpu_resources) { upload_gpu_resources = [] }
   if gpu_upload_usable {
      if diag_on { terminal.log("[gltf] GPU grouped upload enabled: " + to_str(upload_ok) + " parts") }
   } elif diag_on {
      terminal.log("[gltf] GPU grouped upload failed(ok=" + to_str(upload_ok) + " fail=" + to_str(upload_fail) + ")")
   }
   {
      "force_cpu_group": force_cpu_group,
      "gpu_upload_usable": gpu_upload_usable,
      "upload_ok": upload_ok,
      "upload_fail": upload_fail,
      "upload_ms": t_upload_ms,
      "gpu_v_count": int(upload.get("gpu_v_count", 0)),
      "gpu_i_count": int(upload.get("gpu_i_count", 0)),
      "gpu_parts": upload_gpu_parts,
      "gpu_optical_start": int(upload.get("gpu_optical_start", 0)),
      "gpu_blend_start": int(upload.get("gpu_blend_start", 0)),
      "gpu_resources": upload_gpu_resources,
      "has_blend": bool(upload.get("has_blend", false)),
      "has_optical": bool(upload.get("has_optical", false)),
      "packed_upload": bool(upload.get("packed_upload", false))
   }
}

fn _scene_render_has_optical(any render_parts) bool {
   mut render_has_optical = false
   mut ro_i = 0
   def render_parts_n = render_parts.len
   def optical_debug_on = common.env_enabled("NY_GLTF_OPTICAL_DEBUG")
   while ro_i < render_parts_n {
      def ro_part = render_parts.get(ro_i, 0)
      def ro_is_optical = _cpu_part_is_optical(ro_part)
      if optical_debug_on && ro_i < 6 && is_dict(ro_part) {
         def ro_slab = ro_part.get("material_slab", 0)
         def ro_mesh = ro_part.get("mesh", 0)
         def ro_mslab = is_dict(ro_mesh) ? ro_mesh.get("material_slab", 0) : 0
         terminal.log("[gltf:optical] part=" + to_str(ro_i) +
            " slab=" + to_str(bool(ro_slab)) +
            " mslab=" + to_str(bool(ro_mslab)) +
            " alpha=0x" + str.to_hex(ro_slab ? load32(ro_slab, 24) : int(ro_part.get("alpha_u32", 0))) +
            " bsdf0=0x" + str.to_hex(ro_slab ? load32(ro_slab, 36) : int(ro_part.get("bsdf0_u32", 0))) +
            " bsdf5=0x" + str.to_hex(ro_slab ? load32(ro_slab, 148) : int(ro_part.get("bsdf5_u32", 0))) +
         " optical=" + to_str(ro_is_optical))
      }
      if ro_is_optical { render_has_optical = true break }
      ro_i += 1
   }
   render_has_optical
}

fn _scene_store_gpu_upload_state(dict mesh, bool usable, list gpu_parts, any gpu_slab, int optical_start, int blend_start, any resources, bool upload_optical, bool upload_blend, bool render_optical, any lights_slab, int lights_count, bool model_baked) dict {
   def draw_optical = usable ? upload_optical : render_optical
   mesh = _scene_set_fields(mesh, [
         ["gpu_parts", usable ? gpu_parts : []], ["gpu_parts_slab", usable ? gpu_slab : 0],
         ["gpu_parts_count", usable ? gpu_parts.len : 0], ["gpu_optical_start", usable ? optical_start : 0],
         ["gpu_blend_start", usable ? blend_start : 0], ["gpu_resources", usable ? resources : []],
         ["has_optical", draw_optical], ["has_blend", upload_blend],
   ])
   mesh["gpu_draw_state"] = [
      usable ? gpu_slab : 0, usable ? gpu_parts.len : 0,
      usable ? optical_start : 0, usable ? blend_start : 0,
      upload_blend ? 1 : 0, model_baked ? 1 : 0,
      lights_slab, lights_count, draw_optical ? 1 : 0,
   ]
   mesh
}

fn _scene_render_view_stage(list src_parts, dict upload_stage, bool use_fit, f64 sp_sc, f64 fit_tx, f64 fit_ty, f64 fit_tz, f64 sort_cam_x, f64 sort_cam_y, f64 sort_cam_z, bool runtime_deform_parts, int scene_lights_pipeline_count, bool diag_on, bool prof_on) dict {
   def force_cpu_group = bool(upload_stage.get("force_cpu_group", false))
   def gpu_upload_usable = bool(upload_stage.get("gpu_upload_usable", false))
   mut gpu_v_count, gpu_i_count = int(upload_stage.get("gpu_v_count", 0)), int(upload_stage.get("gpu_i_count", 0))
   mut upload_gpu_parts = upload_stage.get("gpu_parts", [])
   if !is_list(upload_gpu_parts) { upload_gpu_parts = [] }
   mut upload_gpu_slab = 0
   mut upload_has_blend = bool(upload_stage.get("has_blend", false))
   def upload_packed = bool(upload_stage.get("packed_upload", false))
   mut group_model_baked = gpu_upload_usable
   mut fit_applied = false
   if gpu_upload_usable {
      if use_fit {
         def t_gpu_prebake0 = ticks()
         def scene_fit_m = _scene_fit_model_mat4(sp_sc, fit_tx, fit_ty, fit_tz)
         upload_gpu_parts = _prebake_scene_gpu_parts(upload_gpu_parts, scene_fit_m)
         _scene_prof_elapsed(prof_on, "gpu_prebake", t_gpu_prebake0)
         fit_applied = true
      }
      if upload_has_blend {
         def t_gpu_sort0 = ticks()
         _sort_gpu_parts_blend_camera(upload_gpu_parts, sort_cam_x, sort_cam_y, sort_cam_z)
         _scene_prof_elapsed(prof_on, "gpu_blend_sort", t_gpu_sort0)
      } else {
         if prof_on {
            def _discard_gpu_blend_sort = _scene_prof_skip(prof_on, "gpu_blend_sort", "no_blend")
         }
      }
      def t_gpu_slab0 = ticks()
      upload_gpu_slab = _pack_scene_gpu_parts_slab(upload_gpu_parts, scene_lights_pipeline_count <= 0)
      _scene_prof_elapsed(prof_on, "gpu_slab", t_gpu_slab0)
   }
   if diag_on && upload_gpu_parts.len > 0 {
      def _first_gpu_rec = upload_gpu_parts.get(0, 0)
      terminal.log("[gltf] gpu_parts_len=" + to_str(upload_gpu_parts.len) + " gpu_rec_len=" + to_str(is_list(_first_gpu_rec) ? _first_gpu_rec.len : -1) + " slab_ptr=" + to_str(bool(upload_gpu_slab) ? 1 : 0))
   }
   mut render_parts = []
   mut render_parts_baked = false
   if force_cpu_group || !gpu_upload_usable {
      def cpu_build = _build_scene_cpu_parts(src_parts, false)
      render_parts = cpu_build.get(0, [])
      gpu_v_count = int(cpu_build.get(1, gpu_v_count))
      gpu_i_count = int(cpu_build.get(2, gpu_i_count))
      upload_has_blend = bool(cpu_build.get(3, upload_has_blend))
      if diag_on {
         def cpu_reason = force_cpu_group ? "forced" : "upload-fallback"
         terminal.log("[gltf] CPU grouped draw enabled(" + cpu_reason + "): " + to_str(render_parts.len) + " parts")
      }
   } elif gpu_upload_usable && upload_packed && upload_gpu_parts.len >= 512 {
      def _discard_render_view = _scene_prof_skip(prof_on, "render_view", "packed_gpu")
      render_parts_baked = fit_applied
   } else {
      def t_render_view0 = ticks()
      render_parts = _build_scene_render_parts(upload_gpu_parts)
      _scene_prof_elapsed(prof_on, "render_view", t_render_view0)
      render_parts_baked = fit_applied
   }
   if use_fit && is_list(render_parts) && render_parts.len > 0 && !render_parts_baked && !runtime_deform_parts {
      def scene_fit_m = _scene_fit_model_mat4(sp_sc, fit_tx, fit_ty, fit_tz)
      render_parts = _prebake_scene_render_parts(render_parts, scene_fit_m)
      render_parts_baked = true
      fit_applied = true
   }
   if upload_has_blend {
      def t_cpu_sort0 = ticks()
      _sort_cpu_parts_blend_camera(render_parts, sort_cam_x, sort_cam_y, sort_cam_z)
      _scene_prof_elapsed(prof_on, "cpu_blend_sort", t_cpu_sort0)
   } else {
      if prof_on {
         def _discard_cpu_blend_sort = _scene_prof_skip(prof_on, "cpu_blend_sort", "no_blend")
      }
   }
   return {
      "render_parts": render_parts,
      "render_parts_baked": render_parts_baked,
      "render_has_optical": _scene_render_has_optical(render_parts),
      "fit_applied": fit_applied,
      "group_model_baked": group_model_baked,
      "gpu_v_count": gpu_v_count,
      "gpu_i_count": gpu_i_count,
      "gpu_parts": upload_gpu_parts,
      "gpu_parts_slab": upload_gpu_slab,
      "has_blend": upload_has_blend
   }
}

fn _scene_load_gltf_stage(str gltf_path, str scene_name, bool diag_on, bool stage_trace, bool prof_on, int t0) dict {
   def gltf_data = gltf.load_gltf_file(gltf_path)
   def prefetched_raw_mesh = 0
   def t_parse_ms = ui_profile.elapsed_ms(t0)
   _scene_stage_ms(stage_trace, "mesh.after_parse", t_parse_ms)
   _scene_prof_ms(prof_on, "parse", t_parse_ms)
   if gltf_data == 0 || (is_dict(gltf_data) && gltf_data.contains("error")) {
      def load_err = is_dict(gltf_data) ? to_str(gltf_data.get("error", "")) : ""
      if load_err.len > 0 {
         terminal.log("Failed to load glTF: " + scene_name + " error=" + load_err)
      } else {
         terminal.log("Failed to load glTF: " + scene_name)
      }
      mut load_errors = []
      if is_dict(gltf_data) { load_errors = gltf_data.get("errors", []) }
      if is_list(load_errors) && load_errors.len > 0 {
         terminal.log("[gltf] validation errors: first=" + to_str(load_errors.get(0,
         "")) + " count=" + to_str(load_errors.len))
      }
      return {"ok": false, "gltf_data": gltf_data, "prefetched_raw_mesh": prefetched_raw_mesh, "parse_ms": t_parse_ms}
   }
   if diag_on { terminal.log("[gltf] Parsed in " + to_str(t_parse_ms) + "ms") }
   if stage_trace && is_dict(gltf_data) {
      def dbg_g = gltf_data.get("gltf", 0)
      _scene_stage(stage_trace, "mesh.parsed base=" + to_str(gltf_data.get("base_path", "")) +
         " source=" + to_str(gltf_data.get("source_path", "")) +
         " meshes=" + to_str(is_dict(dbg_g) ? len(dbg_g.get("meshes", [])) : -1) +
         " nodes=" + to_str(is_dict(dbg_g) ? len(dbg_g.get("nodes", [])) : -1) +
         " accessors=" + to_str(is_dict(dbg_g) ? len(dbg_g.get("accessors", [])) : -1) +
      " bufferViews=" + to_str(is_dict(dbg_g) ? len(dbg_g.get("bufferViews", [])) : -1))
   }
   return {"ok": true, "gltf_data": gltf_data, "prefetched_raw_mesh": prefetched_raw_mesh, "parse_ms": t_parse_ms}
}

fn _scene_material_stage(dict gltf_data, str scene_name, str base_path, bool diag_on, bool stage_trace, bool prof_on) dict {
   if diag_on { terminal.log("[gltf] stage: material infos") }
   def node_perf_limit = _scene_material_build_limit()
   def material_total = _scene_gltf_material_count(gltf_data)
   mut material_infos = []
   if node_perf_limit > 0 && material_total > node_perf_limit {
      material_infos = gltf.gltf_material_infos_limited(gltf_data, node_perf_limit)
   } else {
      material_infos = gltf.gltf_material_infos(gltf_data)
   }
   if node_perf_limit > 0 && material_total > node_perf_limit {
      terminal.log("[gltf] material build cap=" + to_str(node_perf_limit) + " of " + to_str(material_total))
      if material_infos.len > node_perf_limit { material_infos = _scene_prefix_list(material_infos, node_perf_limit) }
      _scene_set_mesh_limit(gltf_data, node_perf_limit)
      gltf_data["__material_infos_limited"] = material_infos
   }
   if diag_on { terminal.log("[gltf] Materials: " + to_str(material_infos.len) + "  base=" + base_path) }
   if diag_on { terminal.log("[gltf] stage: build material records") }
   def t_mat0 = ticks()
   def mat_records = build_material_records(material_infos, base_path)
   def t_mat_ms = ui_profile.elapsed_ms(t_mat0)
   _scene_stage_ms(stage_trace, "mesh.after_materials", t_mat_ms)
   _scene_prof_ms(prof_on, "materials", t_mat_ms, " count=" + to_str(mat_records.len))
   return {"material_infos": material_infos, "mat_records": mat_records, "node_perf_limit": node_perf_limit, "mat_ms": t_mat_ms}
}

fn _scene_mesh_convert_stage(str gltf_path, dict gltf_data, any prefetched_raw_mesh, list mat_records, bool morph_runtime_active, bool deform_runtime_active, bool diag_on, bool stage_trace, bool prof_on) dict {
   def _tcache = ticks()
   def t_mesh0 = ticks()
   if diag_on { terminal.log("[gltf] stage: load geom cache or build mesh") }
   mut mesh = is_dict(prefetched_raw_mesh) ? _clone_cached_mesh(prefetched_raw_mesh) : 0
   def prefetched_mesh_hit = is_dict(mesh)
   if prefetched_mesh_hit {
      if diag_on { terminal.log("[gltf:prefetch] raw mesh hit in " + to_str(ui_profile.elapsed_ms(_tcache)) + "ms") }
   } elif !(morph_runtime_active || deform_runtime_active) {
      mesh = _load_ogeom_cache(gltf_path, mat_records)
   }
   if is_dict(mesh) && mesh.get("cached", false) {
      if diag_on { terminal.log("[gltf:cache] load in " + to_str(ui_profile.elapsed_ms(_tcache)) + "ms") }
   } elif !prefetched_mesh_hit {
      if diag_on { terminal.log("[gltf] stage: gltf_to_mesh_group_indexed") }
      mesh = gltf.gltf_to_mesh_group_indexed(gltf_data, 0, mat_records)
      mut mesh_parts_tmp = []
      if is_dict(mesh) { mesh_parts_tmp = mesh.get("parts", []) }
      if !morph_runtime_active
      && !deform_runtime_active
      && is_dict(mesh)
      && is_list(mesh_parts_tmp)
      && mesh_parts_tmp.len > 0{
         _save_ogeom_cache(gltf_path, mesh)
      }
   }
   if diag_on { terminal.log("[gltf] stage: mesh ready") }
   def t_mesh_ms = ui_profile.elapsed_ms(t_mesh0)
   _scene_stage_ms(stage_trace, "mesh.after_mesh", t_mesh_ms)
   if prof_on {
      mut mesh_parts_prof = []
      if is_dict(mesh) { mesh_parts_prof = mesh.get("parts", []) }
      _scene_prof_ms(prof_on, "mesh", t_mesh_ms, " parts=" + to_str(_scene_safe_count(mesh_parts_prof)))
   }
   return {"mesh": mesh, "mesh_ms": t_mesh_ms}
}

fn _scene_log_grouped_state_debug(any render_parts, int upload_ok, int upload_fail, bool gpu_upload_usable, bool group_model_baked, bool fit_applied, bool render_parts_baked) bool {
   if !_gltf_debug_enabled() { return false }
   mut rp_tx, rp_ty, rp_tz = 0.0, 0.0, 0.0
   if is_list(render_parts) && render_parts.len > 0 {
      def rp0 = render_parts.get(0, 0)
      if is_dict(rp0) {
         def rpm = rp0.get("model", 0)
         if is_list(rpm) && rpm.len >= 18 {
            if is_str(rpm.get(16, 0)) {
               rp_tx, rp_ty, rp_tz = float(rpm.get(12, 0.0)), float(rpm.get(13, 0.0)), float(rpm.get(14, 0.0))
            } elif int(rpm.get(0, 0)) == 4 && int(rpm.get(1, 0)) == 4 {
               rp_tx, rp_ty, rp_tz = float(rpm.get(14, 0.0)), float(rpm.get(15, 0.0)), float(rpm.get(16, 0.0))
            }
         }
      }
   }
   terminal.log(
      "[gltf] grouped_state upload_ok=" + to_str(upload_ok) +
      " upload_fail=" + to_str(upload_fail) +
      " gpu_usable=" + to_str(gpu_upload_usable) +
      " gpu_baked=" + to_str(group_model_baked) +
      " fit_applied=" + to_str(fit_applied) +
      " render_parts=" + to_str(render_parts.len) +
      " render_baked=" + to_str(render_parts_baked) +
      " rp0_t=(" + to_str(rp_tx) + "," + to_str(rp_ty) + "," + to_str(rp_tz) + ")"
   )
   true
}

fn _scene_load_morph_stage(dict gltf_data, any morph_overrides) dict {
   mut data = gltf_data
   mut morph_runtime_active = false
   if is_dict(morph_overrides) {
      def morph_apply = gltf.gltf_apply_morph_weights(data, morph_overrides)
      data, morph_runtime_active = morph_apply.get(0, data), morph_apply.get(1, false) ? true : false
   }
   return {"gltf_data": data, "morph_runtime_active": morph_runtime_active}
}

fn _scene_load_deform_state(dict gltf_data) dict {
   def anim_cnt = gltf.gltf_animation_count(gltf_data)
   def skin_cnt = gltf.gltf_skin_count(gltf_data)
   def morph_cnt = gltf.gltf_morph_target_count(gltf_data)
   return {
      "anim_cnt": anim_cnt,
      "skin_cnt": skin_cnt,
      "morph_cnt": morph_cnt,
      "deform_runtime_active": skin_cnt > 0 || morph_cnt > 0 || anim_cnt > 0
   }
}

fn _scene_mesh_parts_ready_stage(any mesh0, any mesh_parts0, any mat_records, str scene_name, bool diag_on, bool prof_on, int t_mesh_start) dict {
   mut mesh = mesh0
   mut mesh_parts = mesh_parts0
   if !is_dict(mesh) || !mesh_parts {
      terminal.log("Failed to convert glTF mesh: " + scene_name)
      return {"ok": false, "mesh": mesh, "src_parts": [], "mesh_parts": mesh_parts}
   }
   if is_list(mesh_parts) && is_list(mat_records) && mat_records.len > 0 {
      mesh_parts = _apply_mat_records_to_parts(mesh_parts, mat_records)
      mesh["parts"] = mesh_parts
   }
   if diag_on { terminal.log("[gltf] Mesh built in " + to_str(ui_profile.elapsed_ms(t_mesh_start)) + "ms") }
   def t_bounds0 = ticks()
   mesh = _scene_refresh_bounds_from_model_parts(mesh)
   _scene_prof_elapsed(prof_on, "bounds_model", t_bounds0)
   mut src_parts = mesh.get("parts", mesh_parts)
   def t_post0 = ticks()
   src_parts = _scene_tag_parts_scene_name(src_parts, scene_name)
   if is_list(src_parts) { mesh["parts"] = src_parts }
   _scene_prof_elapsed(prof_on, "post_parts", t_post0)
   return {"ok": true, "mesh": mesh, "src_parts": src_parts, "mesh_parts": mesh_parts}
}

fn _scene_cam_fit_seed(any cam3d) list {
   mut fit_fov_deg, fit_aspect = 120.0, 16.0 / 9.0
   mut fit_cam_yaw, fit_cam_pitch = 0.0, 0.0
   if is_list(cam3d) {
      def cam_fov = float(_num_or(cam3d.get(16, fit_fov_deg), fit_fov_deg))
      if cam_fov > 1.0 && cam_fov < 179.0 { fit_fov_deg = cam_fov }
      def cam_w, cam_h = float(_num_or(cam3d.get(20, 0.0), 0.0)), float(_num_or(cam3d.get(21, 0.0), 0.0))
      if cam_w > 1.0 && cam_h > 1.0 { fit_aspect = cam_w / cam_h }
      fit_cam_yaw = float(_num_or(cam3d.get(8, cam3d.get(6, fit_cam_yaw)), fit_cam_yaw))
      fit_cam_pitch = float(_num_or(cam3d.get(9, cam3d.get(7, fit_cam_pitch)), fit_cam_pitch))
   }
   return [fit_fov_deg, fit_aspect, fit_cam_yaw, fit_cam_pitch]
}

fn _scene_fit_orbit_camera(any src_parts, f64 raw_sx, f64 raw_sy, f64 raw_sz, f64 wsx, f64 wsz, bool deform_runtime_active, bool morph_runtime_active, f64 seed_yaw, f64 seed_pitch) list {
   mut fit_cam_yaw, fit_cam_pitch = seed_yaw, seed_pitch
   if deform_runtime_active || morph_runtime_active { return [fit_cam_yaw, fit_cam_pitch] }
   def scene_area_like = max(raw_sx * raw_sy, max(raw_sx * raw_sz, raw_sy * raw_sz))
   def orbit_stats = _scene_orbit_front_stats(src_parts, scene_area_like)
   def orbit_face_x, orbit_face_z = float(orbit_stats.get(0, 0.0)), float(orbit_stats.get(1, 0.0))
   def orbit_face_count, orbit_part_count = int(orbit_stats.get(2, 0)), int(orbit_stats.get(3, 0))
   def orbit_has_blend, orbit_has_backdrop = bool(orbit_stats.get(4, false)), bool(orbit_stats.get(5, false))
   def orbit_pick = _scene_fit_pick_orbit(
      raw_sx, raw_sy, raw_sz,
      wsx, wsz, 1.0,
      orbit_face_x, orbit_face_z,
      orbit_face_count, orbit_part_count,
      false,
   orbit_has_blend || orbit_has_backdrop)
   fit_cam_yaw = float(orbit_pick.get(2, fit_cam_yaw))
   def orbit_raw_max = max(raw_sx, max(raw_sy, raw_sz))
   def orbit_raw_min = min(raw_sx, min(raw_sy, raw_sz))
   def orbit_sheet_like = orbit_raw_max > 0.0001 && orbit_raw_min < orbit_raw_max * 0.30
   def orbit_horiz_max = max(raw_sx, raw_sz)
   def orbit_column_like = orbit_horiz_max > 0.0001 && raw_sy > orbit_horiz_max * 1.8
   def orbit_grid_like = orbit_part_count >= 5 &&
   min(raw_sx, raw_sy) > 0.0001 &&
   raw_sz < min(raw_sx, raw_sy) * 0.55 &&
   abs(raw_sx - raw_sy) < orbit_raw_max * 0.22
   def many_part_orbit_quick = _scene_many_part_orbit_quick(orbit_part_count)
   if (orbit_has_backdrop || orbit_sheet_like || orbit_grid_like) && !orbit_column_like && !many_part_orbit_quick {
      def orbit_use_z = raw_sz <= raw_sx
      def orbit_fg_sign = _scene_sheet_foreground_axis_sign(src_parts, scene_area_like, orbit_use_z)
      if orbit_fg_sign != 0 {
         if orbit_use_z {
            fit_cam_yaw = (orbit_fg_sign > 0) ? 0.0 : 180.0
         } else {
            fit_cam_yaw = (orbit_fg_sign > 0) ? -90.0 : 90.0
         }
      }
   }
   if (orbit_has_blend || orbit_has_backdrop || orbit_sheet_like || orbit_grid_like)&& !orbit_column_like { fit_cam_pitch = 0.0 }
   return [fit_cam_yaw, fit_cam_pitch]
}

fn _scene_initial_fit_stage(any mesh0, any src_parts, any cam3d, dict gltf_data, str scene_name, bool deform_runtime_active, bool morph_runtime_active, bool prof_on) dict {
   def t_fit0 = ticks()
   mut mesh = mesh0
   if !(_scene_mesh_bool(mesh, "loader_bounds_ready", false)
      && _scene_bounds_ok(mesh.get("min", 0), mesh.get("max", 0))
      && is_list(src_parts)){
      mesh = _scene_refresh_bounds_from_parts(mesh)
   }
   def fit = _scene_fit_transform_from_bounds(mesh)
   mesh = fit.get(0, mesh)
   def raw_sx, raw_sy, raw_sz = _scene_fit_info_num(fit, 7, 0.0), _scene_fit_info_num(fit, 8, 0.0), _scene_fit_info_num(fit, 9, 0.0)
   def sp_sc, fit_tx, fit_ty, fit_tz = _scene_fit_info_num(fit, 10, 1.0), _scene_fit_info_num(fit, 11, 0.0), _scene_fit_info_num(fit, 12, 0.0), _scene_fit_info_num(fit, 13, 0.0)
   def wsx, wsy, wsz, wspan = _scene_fit_info_num(fit, 14, 0.0), _scene_fit_info_num(fit, 15, 0.0), _scene_fit_info_num(fit, 16, 0.0), _scene_fit_info_num(fit, 17, 10.0)
   def tcx, tcy, tcz = _scene_fit_info_num(fit, 18, 0.0), _scene_fit_info_num(fit, 19, 0.0), _scene_fit_info_num(fit, 20, 0.0)
   def cam_seed = _scene_cam_fit_seed(cam3d)
   def orbit_cam = _scene_fit_orbit_camera(src_parts, raw_sx, raw_sy, raw_sz, wsx, wsz, deform_runtime_active, morph_runtime_active, float(cam_seed.get(2, 0.0)), float(cam_seed.get(3, 0.0)))
   def solved_fit = _scene_store_solved_fit_camera(mesh,
      fit,
      float(cam_seed.get(0, 120.0)),
      float(cam_seed.get(1, 16.0 / 9.0)),
      float(orbit_cam.get(0, 0.0)),
      float(orbit_cam.get(1, 0.0)),
   "initial")
   mesh = solved_fit.get(0, mesh)
   def fit_cam_x = _scene_fit_info_num(solved_fit, 1, tcx)
   def dcy = _scene_fit_info_num(solved_fit, 2, tcy)
   def fit_cam_z = _scene_fit_info_num(solved_fit, 3, tcz)
   mesh = _scene_try_apply_gltf_camera(mesh, gltf_data, sp_sc, fit_tx, fit_ty, fit_tz, tcx, tcy, tcz, wspan)
   _scene_prof_elapsed(prof_on, "fit", t_fit0)
   def sort_cam_x, sort_cam_y = _scene_mesh_num(mesh, "fit_cam_x", fit_cam_x), _scene_mesh_num(mesh, "fit_cam_y", dcy)
   def sort_cam_z = _scene_mesh_num(mesh, "fit_cam_z", fit_cam_z)
   return {
      "mesh": mesh, "fit": fit, "use_fit": _scene_auto_fit_enabled(),
      "raw_sx": raw_sx, "raw_sy": raw_sy, "raw_sz": raw_sz,
      "sp_sc": sp_sc, "fit_tx": fit_tx, "fit_ty": fit_ty, "fit_tz": fit_tz,
      "wsx": wsx, "wsy": wsy, "wsz": wsz, "wspan": wspan,
      "tcx": tcx, "tcy": tcy, "tcz": tcz, "dcy": dcy,
      "fit_cam_x": fit_cam_x, "fit_cam_z": fit_cam_z,
      "sort_cam_x": sort_cam_x, "sort_cam_y": sort_cam_y, "sort_cam_z": sort_cam_z
   }
}

fn _scene_update_fit_matrices(any M_SP, any M_PT, any M_PS, bool use_fit, f64 fit_tx, f64 fit_ty, f64 fit_tz, f64 sp_sc) bool {
   if M_SP == 0 || M_PT == 0 || M_PS == 0 { return false }
   if use_fit {
      mat4_translate_into(fit_tx, fit_ty, fit_tz, M_PT)
      mat4_scale_into(sp_sc, sp_sc, sp_sc, M_PS)
      mat4_mul_into(M_PT, M_PS, M_SP)
   } else {
      mat4_identity_into(M_PT)
      mat4_identity_into(M_PS)
      mat4_identity_into(M_SP)
   }
   true
}

fn _scene_load_anim_fit_stage(dict mesh0, int anim_cnt, int skin_cnt, int morph_cnt, any scene_lights_pipeline_raw, str scene_name, f64 sp_sc, f64 fit_tx, f64 fit_ty, f64 fit_tz, bool prof_on) dict {
   mut mesh = mesh0
   mut scene_lights_pipeline = scene_lights_pipeline_raw
   if !is_list(scene_lights_pipeline) { scene_lights_pipeline = [] }
   mut load_anim_fit = skin_cnt > 0 || morph_cnt > 0
   if common.env_present("NY_GLTF_LOAD_ANIM_FIT") { load_anim_fit = common.env_enabled("NY_GLTF_LOAD_ANIM_FIT") }
   if (anim_cnt > 0 || skin_cnt > 0 || morph_cnt > 0) && load_anim_fit {
      def t_anim_fit0 = ticks()
      mesh = _scene_apply_gltf_animation(mesh, 0, 0.0)
      mesh = _scene_refresh_bounds_from_parts(mesh)
      if morph_cnt > 0 && skin_cnt == 0 { mesh = _scene_refresh_orbit_from_parts(mesh, true) }
      mesh = _scene_recompute_fit_from_bounds(mesh)
      def fit_scale_live, fit_tx_live = _scene_mesh_num(mesh, "fit_scale", sp_sc), _scene_mesh_num(mesh, "fit_tx", fit_tx)
      def fit_ty_live, fit_tz_live = _scene_mesh_num(mesh, "fit_ty", fit_ty), _scene_mesh_num(mesh, "fit_tz", fit_tz)
      def scene_lights_slab_live = _pack_scene_lights_slab(scene_lights_pipeline, fit_scale_live, fit_tx_live, fit_ty_live, fit_tz_live, true)
      def old_scene_lights_slab = mesh.get("scene_lights_slab", 0)
      if old_scene_lights_slab && old_scene_lights_slab != scene_lights_slab_live { free(old_scene_lights_slab) }
      mesh["scene_lights"] = scene_lights_pipeline
      mesh["scene_lights_slab"] = scene_lights_slab_live
      mesh["scene_lights_count"] = scene_lights_pipeline.len
      def gpu_state_live = mesh.get("gpu_draw_state", 0)
      mesh["gpu_draw_state"] = _scene_gpu_state_set_lights(gpu_state_live, scene_lights_slab_live, scene_lights_pipeline.len)
      _scene_prof_elapsed(prof_on, "anim_fit", t_anim_fit0)
   } elif (anim_cnt > 0 || skin_cnt > 0 || morph_cnt > 0) && prof_on {
      _scene_prof(prof_on, "stage=anim_fit skipped=1 reason=NY_GLTF_LOAD_ANIM_FIT_off")
   }
   mesh
}

fn _scene_load_finish_logs(dict mesh0, any gltf_data, str scene_name, any src_parts, any scene_lights, bool diag_on, bool prof_on, bool stage_trace, int t0, f64 t_parse_ms, f64 t_mat_ms, f64 t_mesh_ms, f64 t_upload_ms, int gpu_v_count, int gpu_i_count, bool gpu_upload_usable, f64 sp_sc, f64 raw_sx, f64 raw_sy, f64 raw_sz, int anim_cnt, int skin_cnt, int morph_cnt) dict {
   mut mesh = mesh0
   mut safe_scene_lights = scene_lights
   if !is_list(safe_scene_lights) { safe_scene_lights = [] }
   if anim_cnt > 0 {
      def adur = _scene_first_anim_duration(gltf_data, anim_cnt)
      mesh["anim_duration"] = adur
      if diag_on { terminal.log("[gltf] Animations: " + to_str(anim_cnt) + "  first dur=" + to_str(adur) + "s") }
   }
   if skin_cnt > 0 || morph_cnt > 0 { if diag_on { terminal.log("[gltf] Deform: skins=" + to_str(skin_cnt) + " morphTargets=" + to_str(morph_cnt)) } }
   if safe_scene_lights.len > 0 && diag_on { terminal.log("[gltf] Lights: " + to_str(safe_scene_lights.len) + " punctual") }
   if diag_on {
      terminal.log(
         "[gltf] " + scene_name
         + ": " + to_str(gpu_v_count) + " verts, "
         + to_str(gpu_i_count) + " inds, "
         + to_str(max(_scene_safe_count(mesh.get("gpu_parts", [])), _scene_safe_count(mesh.get("parts", []))))
         + " parts"
      )
   }
   if prof_on {
      _scene_prof(prof_on, "parse_ms=" + to_str(t_parse_ms) +
         " mat_ms=" + to_str(t_mat_ms) +
         " mesh_ms=" + to_str(t_mesh_ms) +
         " upload_ms=" + to_str(t_upload_ms) +
         " parts_src=" + to_str(src_parts.len) +
         " parts_gpu=" + to_str(_scene_safe_count(mesh.get("gpu_parts", []))) +
         " parts_cpu=" + to_str(_scene_safe_count(mesh.get("parts", []))) +
         " gpu_on=" + to_str(gpu_upload_usable) +
      " total_ms=" + to_str(ui_profile.elapsed_ms(t0)))
   }
   if diag_on { terminal.log("[gltf] fit: scale=" + to_str(sp_sc) + " aabb=(" + to_str(raw_sx) + "x" + to_str(raw_sy) + "x" + to_str(raw_sz) + ")") }
   if diag_on { terminal.log("[gltf] Load complete in " + to_str(ui_profile.elapsed_ms(t0)) + "ms") }
   _scene_stage(stage_trace, "mesh.done total_ms=" + to_str(ui_profile.elapsed_ms(t0)))
   mesh
}

fn load_scene_mesh(str gltf_path, str scene_name="Scene", any cam3d=0, any M_SP=0, any M_PT=0, any M_PS=0, any morph_overrides=0) any {
   "Loads a glTF file, uploads all GPU parts, computes fit transform.
   Returns a mesh group dict with fit_*, gpu_parts, anim_* keys, or 0 on failure.
   If cam3d is provided, positions camera at a good diagonal vantage point."
   _scene_ensure_runtime_caches()
   def diag_on = _scene_diag_enabled()
   def stage_trace = _scene_stage_trace_enabled()
   if diag_on { terminal.log("[gltf] Loading: " + gltf_path) }
   _scene_stage(stage_trace, "mesh.begin name=" + scene_name)
   if stage_trace {
      def dbg_dir = ospath.dirname(gltf_path)
      _scene_stage(stage_trace, "mesh.path is_str=" + to_str(is_str(gltf_path)) +
         " len=" + to_str(gltf_path.len) +
         " dir=" + dbg_dir +
      " dir_base=" + ospath.basename(dbg_dir))
   }
   def prof_on = _scene_prof_enabled()
   def t0 = ticks()
   def load_stage = _scene_load_gltf_stage(gltf_path, scene_name, diag_on, stage_trace, prof_on, t0)
   if !bool(load_stage.get("ok", false)) { return 0 }
   mut gltf_data = load_stage.get("gltf_data", 0)
   def prefetched_raw_mesh = load_stage.get("prefetched_raw_mesh", 0)
   def t_parse_ms = float(load_stage.get("parse_ms", 0.0))
   def morph_stage = _scene_load_morph_stage(gltf_data, morph_overrides)
   gltf_data = morph_stage.get("gltf_data", gltf_data)
   def morph_runtime_active = bool(morph_stage.get("morph_runtime_active", false))
   def deform_state = _scene_load_deform_state(gltf_data)
   def anim_cnt_pre = int(deform_state.get("anim_cnt", 0))
   def skin_cnt_pre = int(deform_state.get("skin_cnt", 0))
   def morph_cnt_pre = int(deform_state.get("morph_cnt", 0))
   def deform_runtime_active = bool(deform_state.get("deform_runtime_active", false))
   def base_path = to_str(gltf_data.get("base_path", ""))
   def _t1 = ticks()
   def mat_stage = _scene_material_stage(gltf_data, scene_name, base_path, diag_on, stage_trace, prof_on)
   def material_infos, mat_records = mat_stage.get("material_infos", []), mat_stage.get("mat_records", [])
   def node_perf_limit, t_mat_ms = int(mat_stage.get("node_perf_limit", 0)), float(mat_stage.get("mat_ms", 0.0))
   def mesh_stage = _scene_mesh_convert_stage(gltf_path, gltf_data, prefetched_raw_mesh, mat_records, morph_runtime_active, deform_runtime_active, diag_on, stage_trace, prof_on)
   mut mesh = mesh_stage.get("mesh", 0)
   def t_mesh_ms = float(mesh_stage.get("mesh_ms", 0.0))
   mut mesh_parts = 0
   if is_dict(mesh) { mesh_parts = mesh.get("parts", 0) }
   def ready_stage = _scene_mesh_parts_ready_stage(mesh, mesh_parts, mat_records, scene_name, diag_on, prof_on, _t1)
   if !bool(ready_stage.get("ok", false)) { return 0 }
   mesh = ready_stage.get("mesh", mesh)
   mesh_parts = ready_stage.get("mesh_parts", mesh_parts)
   mut src_parts = ready_stage.get("src_parts", mesh_parts)
   def upload_stage = _scene_grouped_upload_stage(src_parts, deform_runtime_active, diag_on, stage_trace, prof_on)
   def force_cpu_group, gpu_upload_usable = bool(upload_stage.get("force_cpu_group", false)), bool(upload_stage.get("gpu_upload_usable", false))
   def upload_ok, upload_fail = int(upload_stage.get("upload_ok", 0)), int(upload_stage.get("upload_fail", 0))
   def t_upload_ms = float(upload_stage.get("upload_ms", 0.0))
   mut gpu_v_count, gpu_i_count = int(upload_stage.get("gpu_v_count", 0)), int(upload_stage.get("gpu_i_count", 0))
   mut upload_gpu_parts = upload_stage.get("gpu_parts", [])
   if !is_list(upload_gpu_parts) { upload_gpu_parts = [] }
   mut upload_gpu_slab = 0
   def upload_gpu_optical_start, upload_gpu_blend_start = int(upload_stage.get("gpu_optical_start", 0)), int(upload_stage.get("gpu_blend_start", 0))
   mut upload_gpu_resources = upload_stage.get("gpu_resources", [])
   if !is_list(upload_gpu_resources) { upload_gpu_resources = [] }
   mut upload_has_blend, upload_has_optical = bool(upload_stage.get("has_blend", false)), bool(upload_stage.get("has_optical", false))
   def fit_stage = _scene_initial_fit_stage(mesh, src_parts, cam3d, gltf_data, scene_name, deform_runtime_active, morph_runtime_active, prof_on)
   mesh = fit_stage.get("mesh", mesh)
   def use_fit = bool(fit_stage.get("use_fit", false))
   def raw_sx, raw_sy, raw_sz = float(fit_stage.get("raw_sx", 0.0)), float(fit_stage.get("raw_sy", 0.0)), float(fit_stage.get("raw_sz", 0.0))
   def sp_sc, fit_tx, fit_ty, fit_tz = float(fit_stage.get("sp_sc", 1.0)), float(fit_stage.get("fit_tx", 0.0)), float(fit_stage.get("fit_ty", 0.0)), float(fit_stage.get("fit_tz", 0.0))
   def wsx, wsy, wsz, wspan = float(fit_stage.get("wsx", 0.0)), float(fit_stage.get("wsy", 0.0)), float(fit_stage.get("wsz", 0.0)), float(fit_stage.get("wspan", 10.0))
   def tcz, dcy = float(fit_stage.get("tcz", 0.0)), float(fit_stage.get("dcy", 0.0))
   def sort_cam_x, sort_cam_y = float(fit_stage.get("sort_cam_x", 0.0)), float(fit_stage.get("sort_cam_y", dcy))
   def sort_cam_z = float(fit_stage.get("sort_cam_z", tcz))
   mut scene_lights_pipeline_overrides = 0
   if anim_cnt_pre > 0 { scene_lights_pipeline_overrides = gltf.gltf_sample_animation_merged(gltf_data, 0.0) }
   mut scene_lights_pipeline_raw = _scene_limit_lights(gltf.gltf_scene_punctual_lights(gltf_data, scene_lights_pipeline_overrides), _SCENE_LIGHT_MAX)
   if !is_list(scene_lights_pipeline_raw) { scene_lights_pipeline_raw = [] }
   def scene_lights_pipeline_count = scene_lights_pipeline_raw.len
   if anim_cnt_pre > 0 || deform_runtime_active || morph_runtime_active {
      mesh["anim_raw_parts"] = src_parts
      mesh["anim_cpu_parts_ready"] = false
   }
   def t_render0 = ticks()
   def runtime_deform_parts = morph_runtime_active || deform_runtime_active
   def render_stage = _scene_render_view_stage(src_parts, upload_stage, use_fit, sp_sc, fit_tx, fit_ty, fit_tz, sort_cam_x, sort_cam_y, sort_cam_z, runtime_deform_parts, scene_lights_pipeline_count, diag_on, prof_on)
   mut render_parts = render_stage.get("render_parts", [])
   if !is_list(render_parts) { render_parts = [] }
   def render_parts_baked, render_has_optical = bool(render_stage.get("render_parts_baked", false)), bool(render_stage.get("render_has_optical", false))
   def fit_applied, group_model_baked = bool(render_stage.get("fit_applied", false)), bool(render_stage.get("group_model_baked", gpu_upload_usable))
   gpu_v_count, gpu_i_count = int(render_stage.get("gpu_v_count", gpu_v_count)), int(render_stage.get("gpu_i_count", gpu_i_count))
   upload_gpu_parts, upload_gpu_slab = render_stage.get("gpu_parts", upload_gpu_parts), render_stage.get("gpu_parts_slab", upload_gpu_slab)
   if !is_list(upload_gpu_parts) { upload_gpu_parts = [] }
   upload_has_blend = bool(render_stage.get("has_blend", upload_has_blend))
   mesh["parts"] = render_parts
   mesh["parts_count"] = render_parts.len
   mesh["parts_model_baked"] = render_parts_baked
   mesh["gpu_model_baked"] = group_model_baked
   mesh["fit_applied"] = fit_applied
   def fit_fov_live = _scene_mesh_num(mesh, "fit_cam_fov", 120.0)
   def fit_cz_live = _scene_mesh_num(mesh, "fit_cam_z", tcz)
   if fit_fov_live < 15.0 || fit_fov_live > 120.0 { mesh["fit_cam_fov"] = 120.0 }
   if abs(fit_cz_live) > 1000000.0 {
      mesh = _scene_refresh_bounds_from_parts(mesh)
      mesh = _scene_recompute_fit_from_bounds(mesh)
      if !(deform_runtime_active || morph_runtime_active) { mesh = _scene_refresh_orbit_from_parts(mesh, false) }
   }
   _scene_log_grouped_state_debug(render_parts, upload_ok, upload_fail, gpu_upload_usable, group_model_baked, fit_applied, render_parts_baked)
   if _scene_group_trace_enabled() {
      ui_profile.print_text("[group:scene] force_cpu=" + to_str(force_cpu_group) +
         " upload_ok=" + to_str(upload_ok) +
         " upload_fail=" + to_str(upload_fail) +
         " gpu_usable=" + to_str(gpu_upload_usable) +
         " has_optical=" + to_str(gpu_upload_usable ? upload_has_optical : render_has_optical) +
         " has_blend=" + to_str(upload_has_blend) +
         " render_parts=" + to_str(render_parts.len) +
         " render_baked=" + to_str(render_parts_baked) +
      " gpu_baked=" + to_str(group_model_baked))
   }
   if is_list(cam3d) && cam3d.len >= 10 {
      cam3d[0] = _scene_mesh_num(mesh, "fit_cam_x", 0.0)
      cam3d[1] = _scene_mesh_num(mesh, "fit_cam_y", dcy)
      cam3d[2] = _scene_mesh_num(mesh, "fit_cam_z", tcz)
      cam3d[6] = _scene_mesh_num(mesh, "fit_cam_yaw", 0.0)
      cam3d[7] = _scene_mesh_num(mesh, "fit_cam_pitch", 0.0)
      cam3d[8] = _scene_mesh_num(mesh, "fit_cam_yaw", 0.0)
      cam3d[9] = _scene_mesh_num(mesh, "fit_cam_pitch", 0.0)
   }
   _scene_update_fit_matrices(M_SP, M_PT, M_PS, use_fit, fit_tx, fit_ty, fit_tz, sp_sc)
   def anim_cnt, skin_cnt, morph_cnt = anim_cnt_pre, skin_cnt_pre, morph_cnt_pre
   mut scene_lights = scene_lights_pipeline_raw
   if !is_list(scene_lights) { scene_lights = [] }
   def scene_lights_slab, scene_lights_count = _pack_scene_lights_slab(scene_lights, sp_sc, fit_tx, fit_ty, fit_tz, true), scene_lights.len
   def material_feature_mask = _scene_material_feature_mask_from_infos(material_infos)
   def scene_env_sensitive_materials = band(material_feature_mask, _SCENE_ENV_SENSITIVE_MASK) != 0
   mesh = _scene_set_fields(mesh, [
         ["gltf_data", gltf_data], ["anim_count", anim_cnt], ["skin_count", skin_cnt],
         ["morph_target_count", morph_cnt], ["gltf_path", gltf_path], ["scene_name", scene_name],
         ["mat_records", mat_records], ["scene_lights", scene_lights],
         ["scene_lights_slab", scene_lights_slab], ["scene_lights_count", scene_lights_count],
         ["material_feature_mask", material_feature_mask],
         ["scene_env_sensitive_materials", scene_env_sensitive_materials],
   ])
   mesh = _scene_store_gpu_upload_state(mesh, gpu_upload_usable, upload_gpu_parts, upload_gpu_slab, upload_gpu_optical_start, upload_gpu_blend_start, upload_gpu_resources, upload_has_optical, upload_has_blend, render_has_optical, scene_lights_slab, scene_lights_count, group_model_baked)
   _scene_prof_elapsed(prof_on, "render_parts", t_render0)
   mesh = _scene_load_anim_fit_stage(mesh, anim_cnt, skin_cnt, morph_cnt, scene_lights_pipeline_raw, scene_name, sp_sc, fit_tx, fit_ty, fit_tz, prof_on)
   if anim_cnt > 0 || skin_cnt > 0 || morph_cnt > 0 {
      mesh["anim_playing"] = false
      mesh = _scene_upload_static_pose_gpu(mesh, prof_on)
   }
   mesh = _scene_load_finish_logs(mesh, gltf_data, scene_name, src_parts, scene_lights, diag_on, prof_on, stage_trace, t0, t_parse_ms, t_mat_ms, t_mesh_ms, t_upload_ms, gpu_v_count, gpu_i_count, gpu_upload_usable, sp_sc, raw_sx, raw_sy, raw_sz, anim_cnt, skin_cnt, morph_cnt)
   mesh
}

fn load_scene_path(str gltf_path, str scene_name="", any cam3d=0, any M_SP=0, any M_PT=0, any M_PS=0) any {
   "Loads a scene from an already-resolved glTF/GLB file path."
   def raw_path = str.strip(to_str(gltf_path))
   if raw_path.len == 0 {
      terminal.log("ERROR: glTF asset not found: " + to_str(gltf_path))
      return 0
   }
   mut use_name = str.strip(to_str(scene_name))
   if use_name.len == 0 { use_name = scene_asset_name_from_gltf_path(raw_path) }
   load_scene_mesh(raw_path, use_name, cam3d, M_SP, M_PT, M_PS, 0)
}

fn destroy_scene(any scene) bool {
   "Destroys scene GPU resources without emitting user-facing log text."
   if is_dict(scene) {
      scene_fast_reset()
      render.renderer_wait_idle()
      mesh_group_destroy(scene)
   }
   true
}

fn unload_scene(any scene, str loaded_scene_name="") bool {
   "Destroys scene GPU resources. Texture cache stays hot across scene switches."
   destroy_scene(scene)
   if loaded_scene_name.len > 0 {
      terminal.log(loaded_scene_name + " unloaded")
   }
   else { terminal.log("Scene unloaded") }
   true
}

mut _scene_fast_group = 0
mut _scene_fast_gpu_slab = 0
mut _scene_fast_gpu_count = 0
mut _scene_fast_gpu_optical_start = 0
mut _scene_fast_gpu_blend_start = 0
mut _scene_fast_gpu_has_blend = false
mut _scene_fast_gpu_has_optical = false
mut _scene_fast_gpu_model_baked = false
mut _scene_fast_gpu_single_opaque = false
mut _scene_fast_gpu_light_slab = 0
mut _scene_fast_gpu_light_count = 0
mut _scene_fast_gpu_ready = false
mut _scene_fast_lights_bound = false
mut _scene_fast_mask_bound = false
mut _scene_fast_material_bound = false
mut _scene_fast_material_frame = -1

fn scene_fast_reset(any group=0) bool {
   "Resets cached static scene draw state."
   _scene_fast_group = group
   _scene_fast_gpu_slab = 0
   _scene_fast_gpu_count = 0
   _scene_fast_gpu_optical_start = 0
   _scene_fast_gpu_blend_start = 0
   _scene_fast_gpu_has_blend = false
   _scene_fast_gpu_has_optical = false
   _scene_fast_gpu_model_baked = false
   _scene_fast_gpu_single_opaque = false
   _scene_fast_gpu_light_slab = 0
   _scene_fast_gpu_light_count = 0
   _scene_fast_gpu_ready = false
   _scene_fast_lights_bound = false
   _scene_fast_mask_bound = false
   _scene_fast_material_bound = false
   _scene_fast_material_frame = -1
   true
}

fn _scene_fast_material_current() bool {
   _scene_fast_material_bound && _scene_fast_material_frame == render.renderer_frame_index()
}

fn _scene_fast_note_material_bound() bool {
   _scene_fast_material_bound = true
   _scene_fast_material_frame = render.renderer_frame_index()
   true
}

fn _scene_fast_supported(group) bool {
   if bool(group.get("static_pose_gpu_ready", false)) && !bool(group.get("anim_playing", false)) {
      return true
   }
   int(group.get("anim_count", 0)) <= 0
   && int(group.get("skin_count", 0)) <= 0
   && int(group.get("morph_target_count", 0)) <= 0
}

fn _scene_fast_read_gpu_state(group) bool {
   def gpu_state = group.get("gpu_draw_state", 0)
   if is_list(gpu_state) && gpu_state.len >= 9 {
      _scene_fast_gpu_slab = gpu_state.get(0, 0)
      _scene_fast_gpu_count = int(gpu_state.get(1, 0))
      _scene_fast_gpu_optical_start = int(gpu_state.get(2, 0))
      _scene_fast_gpu_blend_start = int(gpu_state.get(3, 0))
      _scene_fast_gpu_has_blend = int(gpu_state.get(4, 0)) != 0
      _scene_fast_gpu_model_baked = int(gpu_state.get(5, 0)) != 0
      _scene_fast_gpu_light_slab = gpu_state.get(6, 0)
      _scene_fast_gpu_light_count = int(gpu_state.get(7, 0))
      _scene_fast_gpu_has_optical = int(gpu_state.get(8, 0)) != 0
      return true
   }
   if is_list(gpu_state) && gpu_state.len >= 7 {
      _scene_fast_gpu_slab = gpu_state.get(0, 0)
      _scene_fast_gpu_count = int(gpu_state.get(1, 0))
      _scene_fast_gpu_optical_start = int(group.get("gpu_optical_start", 0))
      _scene_fast_gpu_blend_start = int(gpu_state.get(2, 0))
      _scene_fast_gpu_has_blend = int(gpu_state.get(3, 0)) != 0
      _scene_fast_gpu_model_baked = int(gpu_state.get(4, 0)) != 0
      _scene_fast_gpu_light_slab = gpu_state.get(5, 0)
      _scene_fast_gpu_light_count = int(gpu_state.get(6, 0))
      _scene_fast_gpu_has_optical = group.get("has_optical", false) ? true : false
      return true
   }
   _scene_fast_gpu_slab = group.get("gpu_parts_slab", 0)
   _scene_fast_gpu_count = int(group.get("gpu_parts_count", 0))
   _scene_fast_gpu_optical_start = int(group.get("gpu_optical_start", 0))
   _scene_fast_gpu_blend_start = int(group.get("gpu_blend_start", 0))
   _scene_fast_gpu_has_blend = group.get("has_blend", false) ? true : false
   _scene_fast_gpu_has_optical = group.get("has_optical", false) ? true : false
   _scene_fast_gpu_model_baked = group.get("gpu_model_baked", false) ? true : false
   _scene_fast_gpu_light_slab = group.get("scene_lights_slab", 0)
   _scene_fast_gpu_light_count = int(group.get("scene_lights_count", 0))
   true
}

fn _scene_fast_clamp_ranges() bool {
   if _scene_fast_gpu_optical_start < 0 { _scene_fast_gpu_optical_start = 0 }
   if _scene_fast_gpu_optical_start > _scene_fast_gpu_count { _scene_fast_gpu_optical_start = _scene_fast_gpu_count }
   if _scene_fast_gpu_blend_start < 0 { _scene_fast_gpu_blend_start = 0 }
   if _scene_fast_gpu_blend_start > _scene_fast_gpu_count { _scene_fast_gpu_blend_start = _scene_fast_gpu_count }
   true
}

fn _scene_fast_refresh_flags() bool {
   _scene_fast_gpu_ready = bool(_scene_fast_gpu_slab) && _scene_fast_gpu_count > 0
   _scene_fast_gpu_single_opaque = _scene_fast_gpu_ready
   && _scene_fast_gpu_count == 1
   && !_scene_fast_gpu_has_optical
   && !_scene_fast_gpu_has_blend
   && (_scene_fast_gpu_optical_start == 0 || _scene_fast_gpu_optical_start == _scene_fast_gpu_count)
   true
}

fn _scene_fast_log_state(bool enabled) bool {
   if enabled {
      ui_profile.print_line("bench", "gpu slab=0x" + str.to_hex(_scene_fast_gpu_slab) +
         " count=" + to_str(_scene_fast_gpu_count) +
         " optical_start=" + to_str(_scene_fast_gpu_optical_start) +
         " blend_start=" + to_str(_scene_fast_gpu_blend_start) +
         " has_optical=" + to_str(_scene_fast_gpu_has_optical) +
         " has_blend=" + to_str(_scene_fast_gpu_has_blend) +
         " model_baked=" + to_str(_scene_fast_gpu_model_baked) +
      " lights=" + to_str(_scene_fast_gpu_light_count))
   }
   true
}

fn _scene_fast_group_state_matches(any group) bool {
   if to_int(group) != to_int(_scene_fast_group) { return false }
   mut slab = group.get("gpu_parts_slab", 0)
   mut count = int(group.get("gpu_parts_count", 0))
   def gpu_state = group.get("gpu_draw_state", 0)
   if is_list(gpu_state) && gpu_state.len >= 2 {
      slab = gpu_state.get(0, slab)
      count = int(gpu_state.get(1, count))
   }
   to_int(slab) == to_int(_scene_fast_gpu_slab) && count == _scene_fast_gpu_count
}

fn _scene_fast_group_has_packed_state(any group) bool {
   if !is_dict(group) { return false }
   def gpu_state = group.get("gpu_draw_state", 0)
   if is_list(gpu_state) && gpu_state.len >= 7 {
      return bool(gpu_state.get(0, 0)) && int(gpu_state.get(1, 0)) > 0
   }
   bool(group.get("gpu_parts_slab", 0)) && int(group.get("gpu_parts_count", 0)) > 0
}

fn _scene_fast_refresh_cache(group, bool log_enabled=false) bool {
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] cache match check") }
   if _scene_fast_group_state_matches(group) { return true }
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] cache reset") }
   scene_fast_reset(group)
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] support check") }
   if !_scene_fast_supported(group) { return false }
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] read gpu state") }
   _scene_fast_read_gpu_state(group)
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] clamp") }
   _scene_fast_clamp_ranges()
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] flags") }
   _scene_fast_refresh_flags()
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] log state") }
   _scene_fast_log_state(log_enabled)
   true
}

fn _scene_fast_bind_state(any model_matrix) bool {
   if !_scene_fast_lights_bound {
      render.renderer_set_scene_lights_slab(_scene_fast_gpu_light_slab, _scene_fast_gpu_light_count)
      _scene_fast_lights_bound = true
   }
   if _scene_fast_gpu_model_baked {
      render.renderer_set_mask(2)
      render.set_model_matrix(model_matrix)
      _scene_fast_mask_bound = false
   } else {
      if !_scene_fast_mask_bound {
         render.renderer_set_mask(0)
         _scene_fast_mask_bound = true
      }
      render.set_model_matrix(model_matrix)
   }
   true
}

fn _scene_fast_draw_single_opaque() bool {
   mut ok = false
   if _scene_fast_material_current() {
      ok = render.renderer_draw_part0_flat_state_no_restore(_scene_fast_gpu_slab) > 0
   } else {
      ok = render.renderer_draw_part0_flat_no_restore(_scene_fast_gpu_slab) > 0
      if ok { _scene_fast_note_material_bound() }
   }
   ok
}

fn _scene_fast_draw_flat_range(int start_idx, int end_idx, any blend, bool use_material_state) int {
   if end_idx <= start_idx { return 0 }
   if use_material_state {
      return render.renderer_draw_parts_flat_range_state_no_restore(_scene_fast_gpu_slab, start_idx, end_idx, blend)
   }
   render.renderer_draw_parts_flat_range_no_restore(_scene_fast_gpu_slab, start_idx, end_idx, blend)
}

fn _scene_fast_draw_multi_static_ranges() bool {
   mut drawn = 0
   def use_material_state = _scene_fast_material_current() && !_scene_fast_gpu_has_optical && !_scene_fast_gpu_has_blend
   drawn += _scene_fast_draw_flat_range(0, _scene_fast_gpu_optical_start, 0, use_material_state)
   if _scene_fast_gpu_has_optical && _scene_fast_gpu_optical_start < _scene_fast_gpu_count {
      render.renderer_capture_scene_color_resume_pass()
   }
   drawn += _scene_fast_draw_flat_range(_scene_fast_gpu_optical_start, _scene_fast_gpu_blend_start, 0, use_material_state)
   if _scene_fast_gpu_has_blend && _scene_fast_gpu_blend_start < _scene_fast_gpu_count {
      drawn += _scene_fast_draw_flat_range(_scene_fast_gpu_blend_start, _scene_fast_gpu_count, 1, false)
   } elif drawn <= 0 && _scene_fast_gpu_count > 0 {
      drawn += _scene_fast_draw_flat_range(0, _scene_fast_gpu_count, 0, use_material_state)
   }
   if _scene_fast_gpu_has_optical { render.renderer_clear_scene_color_capture() }
   if !use_material_state && drawn > 0 && !_scene_fast_gpu_has_optical && !_scene_fast_gpu_has_blend {
      _scene_fast_note_material_bound()
   }
   drawn > 0
}

fn _scene_fast_draw_generic(any group, any model_matrix) bool {
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] generic enter") }
   render.set_model_matrix(model_matrix)
   def ok = render.draw_mesh_group(group)
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] generic exit ok=" + to_str(ok)) }
   ok
}

fn _scene_fast_group_deform_active(any group) bool {
   if !is_dict(group) { return false }
   def has_anim = int(group.get("anim_count", 0)) > 0 || int(group.get("skin_count", 0)) > 0 || int(group.get("morph_target_count", 0)) > 0
   if !has_anim { return false }
   bool(group.get("anim_playing", false)) || bool(group.get("anim_time_override", false))
}

fn scene_fast_draw(any group, any model_matrix, bool log_enabled=false) bool {
   "Draws a cached static GPU scene when compatible."
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] enter") }
   if !is_dict(group) { return false }
   if _scene_fast_group_deform_active(group) { return _scene_fast_draw_generic(group, model_matrix) }
   if !render.renderer_packed_scene_supported() {
      if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] packed unsupported") }
      return _scene_fast_draw_generic(group, model_matrix)
   }
   if !_scene_fast_group_has_packed_state(group) {
      if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] no packed state") }
      return _scene_fast_draw_generic(group, model_matrix)
   }
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] refresh") }
   if !_scene_fast_refresh_cache(group, log_enabled) {
      if log_enabled { ui_profile.print_text("[scene:fast] unsupported") }
      if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] refresh unsupported -> generic") }
      return _scene_fast_draw_generic(group, model_matrix)
   }
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] ready=" + to_str(_scene_fast_gpu_ready) + " count=" + to_str(_scene_fast_gpu_count) + " slab=" + to_str(_scene_fast_gpu_slab)) }
   if !_scene_fast_gpu_ready {
      if log_enabled {
         ui_profile.print_text("[scene:fast] not-ready slab=" + to_str(_scene_fast_gpu_slab != 0) +
         " count=" + to_str(_scene_fast_gpu_count))
      }
      return _scene_fast_draw_generic(group, model_matrix)
   }
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] bind") }
   _scene_fast_bind_state(model_matrix)
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] packed draw") }
   def ok = _scene_fast_gpu_single_opaque ? _scene_fast_draw_single_opaque() : _scene_fast_draw_multi_static_ranges()
   if ui_profile.env_truthy_cached("NY_SCENE_FAST_TRACE") { ui_profile.print_text("[scene:fast] packed ok=" + to_str(ok)) }
   ok ? true : _scene_fast_draw_generic(group, model_matrix)
}

fn scene_fast_color_reuse_ready(any group) bool {
   "Reports whether the current cached scene can reuse the static color target."
   if _scene_fast_group_deform_active(group) { return false }
   to_int(group) == to_int(_scene_fast_group)
   && _scene_fast_gpu_single_opaque
   && !_scene_fast_gpu_has_blend
   && !_scene_fast_gpu_has_optical
   && _scene_fast_gpu_count == 1
}

#main {
   assert(scene_asset_name_from_gltf_path("assets/models/Fox/Fox.gltf") == "Fox", "scene direct gltf asset name")
   assert(scene_asset_name_from_gltf_path("assets/models/Fox/glTF/Fox.gltf") == "Fox", "scene nested gltf asset name")
   assert(format_name_list(["Fox", "", "Cube"]) == "Fox, Cube", "scene name list formatting")
   assert(apply_gltf_animation(0) == 0, "scene animation no-op")
   def fitted = {"fit_applied": true, "gpu_model_baked": false}
   assert(scene_apply_fit(fitted).get("fit_applied", false), "scene fit no-op")
   mut state = _scene_bounds_accum_new()
   state = _scene_bounds_accum(state, [-1.0, 2.0, 0.5], [3.0, 4.0, 5.0])
   state = _scene_bounds_accum(state, [-2.0, 3.0, -7.0], [2.0, 8.0, 1.0])
   def direct = _scene_bounds_accum_result(state)
   assert(direct.get(0).get(0) == -2.0 && direct.get(0).get(2) == -7.0 && direct.get(1).get(1) == 8.0, "scene direct bounds accum")
   def part = {"min": [10.0, 11.0, 12.0], "max": [13.0, 14.0, 15.0]}
   state = _scene_bounds_accum_new()
   state = _scene_bounds_accum_part(state, part)
   def fallback = _scene_bounds_accum_result(state)
   assert(fallback.get(0).get(0) == 10.0 && fallback.get(1).get(2) == 15.0, "scene part bounds fallback")
   state = _scene_bounds_accum_new()
   state = _scene_bounds_accum_part(state, part, [[-4.0, -3.0, -2.0], [6.0, 7.0, 8.0]])
   def baked = _scene_bounds_accum_result(state)
   assert(baked.get(0).get(0) == -4.0 && baked.get(1).get(2) == 8.0, "scene baked bounds")
   def translated = _scene_transform_aabb(
      [1.0, 2.0, 3.0],
      [2.0, 3.0, 4.0],
      [4, 4,
         1.0, 0.0, 0.0, 0.0,
         0.0, 1.0, 0.0, 0.0,
         0.0, 0.0, 1.0, 0.0,
      10.0, 20.0, 30.0, 1.0]
   )
   assert(translated.get(0).get(0) == 11.0 && translated.get(1).get(2) == 34.0, "scene transform aabb")
   mut drag_scene = {"edit_tx": 0.0, "edit_ty": 0.0, "edit_tz": 0.0}
   mut drag = scene_drag_begin_state(drag_scene, 10.0, 20.0, 0)
   drag = scene_drag_apply(drag_scene, drag, 30.0, 10.0, 0.0, [0, 0, 0, 10, 1, 1])
   assert(bool(drag.get("changed", false)) && float(drag_scene.get("edit_tx", 0.0)) > 0.0 && float(drag_scene.get("edit_ty", 0.0)) > 0.0, "scene drag translate")
   mut huge_drag_scene = {"edit_tx": 0.0, "edit_ty": 0.0, "edit_tz": 0.0}
   mut huge_drag = scene_drag_begin_state(huge_drag_scene, 0.0, 0.0, 0, {"axis": 2, "screen_axis_x": 0.0, "screen_axis_y": -1.0})
   huge_drag = scene_drag_apply(huge_drag_scene, huge_drag, 0.0, -140.0, 0.0, [0, 0, 0, 100000, 1, 1])
   assert(float(huge_drag_scene.get("edit_ty", 0.0)) > 0.0 && float(huge_drag_scene.get("edit_ty", 0.0)) < 5.0, "scene drag huge Y is stable")
   mut cam_drag_scene = {"edit_tx": 0.0, "edit_ty": 0.0, "edit_tz": 0.0}
   mut cam_drag = scene_drag_begin_state(cam_drag_scene, 100.0, 100.0, 0, {
         "drag_world_per_pixel": 0.10,
         "drag_right_x": 1.0, "drag_right_y": 0.0, "drag_right_z": 0.0,
         "drag_up_x": 0.0, "drag_up_y": 1.0, "drag_up_z": 0.0
   })
   cam_drag = scene_drag_apply(cam_drag_scene, cam_drag, 110.0, 80.0, 180.0, [0, 0, 0, 1, 1, 1], 99.0, 88.0, 77.0, 30.0, -45.0)
   assert(abs(float(cam_drag_scene.get("edit_tx", 0.0)) - 1.0) < 0.00001 && abs(float(cam_drag_scene.get("edit_ty", 0.0)) - 2.0) < 0.00001, "scene drag uses frozen camera plane")
   mut axis_drag_scene = {"edit_tx": 0.0, "edit_ty": 0.0, "edit_tz": 0.0}
   mut axis_drag = scene_drag_begin_state(axis_drag_scene, 100.0, 100.0, 0, {
         "axis": 1, "screen_axis_x": 0.0, "screen_axis_y": -1.0,
         "axis_world_per_pixel": 0.10, "drag_world_per_pixel": 0.10
   })
   axis_drag = scene_drag_apply(axis_drag_scene, axis_drag, 100.0, 90.0, 0.0, [0, 0, 0, 1, 1, 1])
   assert(abs(float(axis_drag_scene.get("edit_tx", 0.0)) - 1.0) < 0.00001, "scene drag uses frozen axis tangent")
   mut y_ray_scene = {"edit_tx": 0.0, "edit_ty": 0.0, "edit_tz": 0.0}
   mut y_ray_drag = scene_drag_begin_state(y_ray_scene, 0.0, 0.0, 0, {
         "axis": 2, "screen_axis_x": 0.0, "screen_axis_y": -1.0,
         "axis_world_per_pixel": 0.01, "drag_world_per_pixel": 0.01
   })
   y_ray_drag["axis_world_delta_ok"] = true
   y_ray_drag["axis_world_delta"] = 3.25
   y_ray_drag = scene_drag_apply(y_ray_scene, y_ray_drag, 0.0, -1.0, 0.0, [0, 0, 0, 1, 1, 1])
   assert(abs(float(y_ray_scene.get("edit_ty", 0.0)) - 3.25) < 0.00001 && to_str(y_ray_drag.get("drag_axis_source", "")) == "ray", "scene Y drag prefers ray world delta")
   mut stale_ray_scene = {"edit_tx": 0.0, "edit_ty": 0.0, "edit_tz": 0.0}
   mut stale_ray_drag = scene_drag_begin_state(stale_ray_scene, 0.0, 0.0, 0, {
         "axis": 2, "screen_axis_x": 0.0, "screen_axis_y": -1.0,
         "axis_world_per_pixel": 0.01, "drag_world_per_pixel": 0.01
   })
   stale_ray_drag["axis_world_delta_ok"] = true
   stale_ray_drag["axis_world_delta"] = 99.0
   stale_ray_drag["ray_update_ok"] = false
   stale_ray_drag = scene_drag_apply(stale_ray_scene, stale_ray_drag, 0.0, -10.0, 0.0, [0, 0, 0, 1, 1, 1])
   assert(abs(float(stale_ray_scene.get("edit_ty", 0.0)) - 0.10) < 0.00001 && to_str(stale_ray_drag.get("drag_axis_source", "")) == "screen", "scene drag ignores stale failed ray delta")
   drag = scene_drag_begin_state(drag_scene, 0.0, 0.0, 2)
   drag = scene_drag_apply(drag_scene, drag, 10.0, -10.0, 0.0, [])
   assert(float(drag_scene.get("edit_scale", 1.0)) > 1.0, "scene drag scale")
   def tweak_scene = {
      "mat_records": [{"base_color_u32": 0xffffffff, "material_u32": 0x0000ff00}],
      "parts": [{"mat_idx": 0, "base_color_u32": 0xffffffff, "material_u32": 0x0000ff00}],
      "gpu_parts": [[-1, 1.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0xffffffff, 0x0000ff00, -1, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, [0, 0, 0], [1, 1, 1], 0, 0, 0, 0x80000000, 0, 0, 0, 0]],
      "gltf_data": {"gltf": {"materials": [{"pbrMetallicRoughness": {}}]}}
   }
   def tweaked = scene_apply_material_tweak(tweak_scene, 0, [0.25, 0.5, 0.75, 1.0], 0.2, 0.8)
   assert(int(tweaked.get("material_tweak_revision", 0)) == 1 &&
      int(tweaked.get("mat_records", [dict(0)]).get(0).get("base_color_u32", 0)) != 0xffffffff &&
      int(tweaked.get("parts", [dict(0)]).get(0).get("material_u32", 0)) == int(tweaked.get("mat_records", [dict(0)]).get(0).get("material_u32", -1)) &&
      int(tweaked.get("gpu_parts", [[]]).get(0).get(13, -1)) == int(tweaked.get("mat_records", [dict(0)]).get(0).get("material_u32", -2)),
   "scene material tweak updates records and parts")
   def tex = {"compare_env": 10, "compare_env_spec": 11, "compare_reflect_spec": 12, "compare_visible_env": 13, "neutral_env": 20, "neutral_env_spec": 21, "skybox": 30, "skybox_spec": 31}
   assert(first_ready([-1, 0, 2]) == 0 && first_ready([]) == -1, "scene env first ready")
   assert(mode_flags(false, false, false, false, true, false, 0) == [false, false, true, false, true], "scene env optical mode")
   assert(mode_flags(false, false, false, false, false, false, 1) == [true, false, true, false, false], "scene env forced studio")
   assert(scene_material_info({"material_feature_mask": 144, "scene_env_sensitive_materials": true}, false, false) == [144, true, true, true, true], "scene env material info")
   assert(generated_plan({}, true, false, false, 0, true, false, false, false, false, false) == [true, true, false, false, false, false], "scene env generated plan missing")
   assert(generated_plan(tex, true, false, false, 0, true, false, false, false, false, false).get(0) == false, "scene env generated plan ready")
   assert(background_requested(false, false, false, 2, false, false, false, false, false, false, false, false), "scene env background neutral")
   assert(!background_requested(false, true, false, 0, true, false, true, false, false, false, false, false), "scene env probe hides background")
   assert(base_pair(tex, true, false, true, false, false, false) == [10, 12], "scene env studio pair")
   assert(scene_fallbacks(tex, -1, -1, true, true, false, false) == [20, 12], "scene env scene fallback")
   assert(apply_overrides(tex, 10, 11, false, 4, false) == [-1, -1], "scene env off override")
   assert(scene_override_pair(tex, true, false, true, false, false, false, true, true, false, false, 0, false) == [10, 12], "scene env scene override")
   assert(normalize_pair(tex, 30, 11, false, false, false) == [30, 11], "scene env normalize pair")
   assert(!proof_needs_generated(tex, -1, -1, true, false, true, true), "scene env proof generation ready")
   assert(proof_fallbacks(tex, -1, -1, true, false, true, true, false, false) == [20, 12], "scene env proof fallback")
   assert(skybox_fallback(tex, -1, -1, true) == [30, 31], "scene env skybox fallback")
   assert(scene_fast_reset(), "scene fast reset")
   assert(!scene_fast_draw(0, 0), "scene fast rejects missing scene")
   mut posed = {"anim_count": 1, "skin_count": 1, "morph_target_count": 0, "static_pose_gpu_ready": true, "anim_playing": false}
   assert(_scene_fast_supported(posed), "scene fast accepts static skinned pose")
   posed["anim_playing"] = true
   assert(!_scene_fast_supported(posed), "scene fast rejects playing skinned pose")
   assert(!scene_fast_color_reuse_ready(0), "scene fast reuse cold")
   print("✓ std.os.ui.render.scene self-test passed")
}
