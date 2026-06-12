;; Keywords: render env scene material lighting os ui
;; Backend-neutral scene/environment preference helpers.
module std.os.ui.render.env(
   scene_prefers_studio_env, scene_prefers_neutral_env,
   scene_prefers_compare_reflect_env, scene_prefers_compare_visible_env,
   scene_prefers_optical_spec_env, scene_prefers_black_visible_env,
   scene_prefers_gray_proof_bg
)

use std.core
use std.core.str as str
use std.os.prim (env)

fn _scene_key(any name) str { str.lower(str.strip(to_str(name))) }

fn _scene_has_any(str s, list words) bool {
   mut i = 0
   while(i < words.len){
      def w = str.lower(str.strip(to_str(words.get(i, ""))))
      if(w.len > 0 && str.str_contains(s, w)){ return true }
      i += 1
   }
   false
}

fn _scene_env_match(any name, str env_name) bool {
   def raw = _scene_key(env(env_name))
   if(raw.len == 0){ return false }
   if(raw == "*"){ return true }
   def s = _scene_key(name)
   if(s.len == 0){ return false }
   def parts = str.split(raw, ",")
   mut i = 0
   while(i < parts.len){
      def p = str.strip(to_str(parts.get(i, "")))
      if(p.len > 0 && (s == p || str.str_contains(s, p))){ return true }
      i += 1
   }
   false
}

fn _scene_env_override(any name, str on_env, str off_env) int {
   if(off_env.len > 0 && _scene_env_match(name, off_env)){ return 0 }
   if(on_env.len > 0 && _scene_env_match(name, on_env)){ return 1 }
   -1
}

fn scene_prefers_studio_env(any name) bool {
   "Returns whether a scene should use a studio environment by default."
   def ov = _scene_env_override(name, "NY_UI_SCENE_STUDIO_ENV", "NY_UI_SCENE_NO_STUDIO_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   if(s.len == 0){ return false }
   if(str.startswith(s, "compare") || str.endswith(s, "testgrid")){ return false }
   str.endswith(s, "spheres") ||
   _scene_has_any(s, ["metal", "rough", "spec", "gloss", "sheen", "clearcoat", "anisotropy", "iridescence", "pbr", "carpaint", "velvet", "leather"])
}

fn scene_prefers_neutral_env(any name) bool {
   "Returns whether a scene should use a neutral environment by default."
   def ov = _scene_env_override(name, "NY_UI_SCENE_NEUTRAL_ENV", "NY_UI_SCENE_NO_NEUTRAL_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   str.startswith(s, "compare") ||
   _scene_has_any(s, ["transmission", "volume", "ior", "dispersion", "attenuation", "glass", "scatter", "diffuse", "light", "emissive", "environment", "texture", "uv", "normal", "sheen", "specular", "metallic", "roughness"])
}

fn scene_prefers_compare_reflect_env(any name) bool {
   "Returns whether a scene should use the compare-reflection environment."
   def ov = _scene_env_override(name, "NY_UI_SCENE_REFLECT_ENV", "NY_UI_SCENE_NO_REFLECT_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["metal", "rough", "spec", "gloss", "iridescence", "sheen", "clearcoat", "anisotropy", "reflect", "environment", "pbr"])
}

fn scene_prefers_compare_visible_env(any name) bool {
   "Returns whether a scene should use the compare-visible environment."
   def ov = _scene_env_override(name, "NY_UI_SCENE_VISIBLE_ENV", "NY_UI_SCENE_NO_VISIBLE_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["transmission", "glass", "visible", "scatter", "sunglass", "transparent", "environment"])
}

fn scene_prefers_optical_spec_env(any name) bool {
   "Returns whether a scene should use the optical/specular environment."
   def ov = _scene_env_override(name, "NY_UI_SCENE_OPTICAL_ENV", "NY_UI_SCENE_NO_OPTICAL_ENV")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["transmission", "volume", "ior", "dispersion", "attenuation", "glass", "water", "transparent", "iridescence", "optical", "diffuse"])
}

fn scene_prefers_black_visible_env(any name) bool {
   "Returns whether visible-environment rendering should use a black visible background."
   _scene_env_match(name, "NY_UI_SCENE_BLACK_VISIBLE_ENV") &&
   !_scene_env_match(name, "NY_UI_SCENE_NO_BLACK_VISIBLE_ENV")
}

fn scene_prefers_gray_proof_bg(any name) bool {
   "Returns whether proof captures should prefer a neutral gray background."
   def ov = _scene_env_override(name, "NY_UI_SCENE_GRAY_BG", "NY_UI_SCENE_NO_GRAY_BG")
   if(ov >= 0){ return ov == 1 }
   def s = _scene_key(name)
   _scene_has_any(s, ["test", "helmet", "shoe", "lamp", "texture", "rough", "ior", "meshopt", "light", "cloth", "fabric", "carbon", "glass", "transmission", "dispersion", "attenuation"])
}
