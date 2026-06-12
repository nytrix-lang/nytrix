;; Keywords: engine environment skybox lighting view os ui render viewer scene
;; Environment, skybox, lighting, and viewport background controls for the engine viewer.
;; References:
;; - std.os.ui.assets.viewer
;; - std.os.ui.render.scene
module std.os.ui.render.viewer.engine.env(load_skybox, build_generated_textures)
use std.core
use std.core.str as str
use std.os.fs as osfs
use std.os.ui.assets.viewer as ui_assets
use std.os.ui.render.dump as ui_profile
use std.os.ui.render
use std.os.ui.render.scene as render_scene
use std.os.ui.render.viewer.term as terminal
use std.os.ui.render.viewer.engine.state
use std.parse.img as img
use std.parse.img.exr as exr

fn _reset_skybox_texture_resources() {
   if(skybox_tex_id >= 0){
      texture_destroy(skybox_tex_id)
      if(skybox_spec_tex_id == skybox_tex_id){
         skybox_spec_tex_id = -1
      }
      if(compare_env_tex_id == skybox_tex_id){
         compare_env_tex_id = -1
         compare_env_spec_tex_id = -1
      }
      skybox_tex_id = -1
   }
   if(skybox_im_hold && is_dict(skybox_im_hold)){
      img.free(skybox_im_hold)
      skybox_im_hold = 0
   }
   if(skybox_spec_tex_id >= 0){
      texture_destroy(skybox_spec_tex_id)
      skybox_spec_tex_id = -1
   }
   true
}

fn _reset_compare_env_resources() {
   if(compare_env_tex_id >= 0){
      texture_destroy(compare_env_tex_id)
      if(compare_env_spec_tex_id == compare_env_tex_id){
         compare_env_spec_tex_id = -1
      }
      compare_env_tex_id = -1
   }
   if(compare_env_im_hold && is_dict(compare_env_im_hold)){
      img.free(compare_env_im_hold)
      compare_env_im_hold = 0
   }
   if(compare_env_spec_tex_id >= 0){
      texture_destroy(compare_env_spec_tex_id)
      compare_env_spec_tex_id = -1
   }
   if(compare_reflect_spec_tex_id >= 0){
      texture_destroy(compare_reflect_spec_tex_id)
      compare_reflect_spec_tex_id = -1
   }
   if(compare_reflect_env_im_hold && is_dict(compare_reflect_env_im_hold)){
      img.free(compare_reflect_env_im_hold)
      compare_reflect_env_im_hold = 0
   }
   true
}

fn _reset_compare_visible_env_resources() {
   if(compare_visible_env_tex_id >= 0){
      texture_destroy(compare_visible_env_tex_id)
      compare_visible_env_tex_id = -1
   }
   if(compare_visible_env_im_hold && is_dict(compare_visible_env_im_hold)){
      img.free(compare_visible_env_im_hold)
      compare_visible_env_im_hold = 0
   }
   true
}

fn _reset_neutral_env_resources() {
   if(neutral_env_tex_id >= 0){
      texture_destroy(neutral_env_tex_id)
      if(neutral_env_spec_tex_id == neutral_env_tex_id){
         neutral_env_spec_tex_id = -1
      }
      neutral_env_tex_id = -1
   }
   if(neutral_env_im_hold && is_dict(neutral_env_im_hold)){
      img.free(neutral_env_im_hold)
      neutral_env_im_hold = 0
   }
   if(neutral_env_spec_tex_id >= 0){
      texture_destroy(neutral_env_spec_tex_id)
      neutral_env_spec_tex_id = -1
   }
   true
}


fn _env_keep_cpu_images() bool {
   ui_profile.env_truthy_cached("NY_UI_KEEP_ENV_CPU_IMAGES")
}

fn _reset_skybox_resources() {
   _reset_skybox_texture_resources()
   _reset_compare_env_resources()
   _reset_compare_visible_env_resources()
   _reset_neutral_env_resources()
   true
}

fn _load_skybox_source_tex(path, verbose) {
   if(!osfs.is_file(path)){
      skybox_tex_id = -1
      skybox_spec_tex_id = -1
      skybox_enabled = false
      if(verbose){
         terminal.log("skybox missing: " + path)
      }
      return -2
   }
   if(str.endswith(str.lower(path), ".exr")){
      def exr_im = exr.load_path(path)
      if(exr_im && is_dict(exr_im)){
         return texture_upload_image_ex(exr_im, path, 37, false, false, 1, 10497, 33071, "", true)
      }
      if(verbose){
         terminal.log("skybox exr decode failed: " + to_str(exr.last_error()))
      }
      return -1
   }
   texture_load_ex(path, 37, true, true, 1, 10497, 33071)
}

fn _install_skybox_tex(tex, visible) {
   skybox_tex_id = tex
   skybox_spec_tex_id = tex
   skybox_enabled = visible ? true : false
   true
}

fn _build_skybox_studio_env(env_w, env_h) {
   def studio_im = generate_studio_env_image(env_w, env_h)
   if(studio_im && is_dict(studio_im)){
      def studio_tex = render_scene.upload_generated_texture(studio_im)
      if(studio_tex >= 0){
         compare_env_tex_id = studio_tex
         compare_env_spec_tex_id = studio_tex
      }
      if(_env_keep_cpu_images()){
         compare_env_im_hold = studio_im
      } else {
         img.free(studio_im)
         compare_env_im_hold = 0
      }
   }
   true
}

fn _build_compare_reflect_env(env_w, env_h) {
   if(!ui_profile.env_truthy_cached("NY_UI_ENABLE_COMPARE_REFLECT_SPEC")){
      return false
   }
   def compare_spec_im = generate_compare_reflect_env_image(env_w, env_h)
   if(compare_spec_im && is_dict(compare_spec_im)){
      def compare_spec_tex = render_scene.upload_generated_texture(compare_spec_im)
      if(compare_spec_tex >= 0){
         compare_reflect_spec_tex_id = compare_spec_tex
         if(_env_keep_cpu_images()){
            compare_reflect_env_im_hold = compare_spec_im
         } else {
            img.free(compare_spec_im)
            compare_reflect_env_im_hold = 0
         }
         return true
      }
      img.free(compare_spec_im)
   }
   false
}

fn _build_compare_visible_env(env_w, env_h) {
   def compare_visible_im = generate_compare_visible_env_image(env_w, env_h)
   if(compare_visible_im && is_dict(compare_visible_im)){
      def compare_visible_tex = render_scene.upload_generated_texture(compare_visible_im)
      if(compare_visible_tex >= 0){
         compare_visible_env_tex_id = compare_visible_tex
      }
      if(_env_keep_cpu_images()){
         compare_visible_env_im_hold = compare_visible_im
      } else {
         img.free(compare_visible_im)
         compare_visible_env_im_hold = 0
      }
      return compare_visible_tex >= 0
   }
   false
}

fn _build_neutral_env(env_w, env_h) {
   def neutral_im = generate_neutral_env_image(env_w, env_h)
   if(neutral_im && is_dict(neutral_im)){
      def neutral_tex = render_scene.upload_generated_texture(neutral_im)
      if(neutral_tex >= 0){
         neutral_env_tex_id = neutral_tex
         neutral_env_spec_tex_id = neutral_tex
      }
      if(_env_keep_cpu_images()){
         neutral_env_im_hold = neutral_im
      } else {
         img.free(neutral_im)
         neutral_env_im_hold = 0
      }
      return neutral_tex >= 0
   }
   false
}

fn _build_skybox_generated_envs(env_w, env_h) {
   _build_skybox_studio_env(env_w, env_h)
   _build_compare_reflect_env(env_w, env_h)
   _build_compare_visible_env(env_w, env_h)
   _build_neutral_env(env_w, env_h)
   true
}

fn build_generated_textures(studio=true, compare_visible=true, neutral=true, compare_reflect=false) bool {
   "Builds missing generated environment textures requested by the viewer world planner."
   def env_w = ui_profile.generated_env_width(false, false)
   def env_h = max(1, env_w / 2)
   if(bool(studio) && compare_env_tex_id < 0){ _build_skybox_studio_env(env_w, env_h) }
   if(bool(compare_visible) && compare_visible_env_tex_id < 0){ _build_compare_visible_env(env_w, env_h) }
   if(bool(neutral) && neutral_env_tex_id < 0){ _build_neutral_env(env_w, env_h) }
   if(bool(compare_reflect) && compare_reflect_spec_tex_id < 0){ _build_compare_reflect_env(env_w, env_h) }
   true
}

fn load_skybox(visible=true, source="") {
   "Loads the viewer skybox and builds the derived environment textures."
   def verbose = ui_profile.env_truthy_cached("NY_UI_STARTUP_TRACE")
   def source_arg = ui_assets.skybox_source_arg(source)
   if(ui_assets.skybox_source_is_off(source_arg)){
      _reset_skybox_resources()
      skybox_enabled = false
      ui_profile.set_bool("NY_UI_PROOF_SKYBOX", false)
      if(verbose){ terminal.log("skybox disabled") }
      return false
   }
   if(visible && source_arg.len > 0){
      ui_profile.set_bool("NY_UI_PROOF_SKYBOX", true)
   }
   if(ui_assets.skybox_source_is_generated(source_arg)){
      _reset_skybox_resources()
      def ok = _load_fast_generated_skybox(visible)
      if(verbose){ terminal.log(ok ? "skybox generated: tex=" + to_str(skybox_tex_id) : "skybox generated failed") }
      return ok
   }
   def path = ui_assets.skybox_resolve_source_path(source_arg)
   _reset_skybox_resources()
   def env_size = ui_assets.skybox_env_size(false)
   def env_w = env_size.get(0, 1024)
   def env_h = env_size.get(1, 512)
   def tex = _load_skybox_source_tex(path, verbose)
   if(tex == -2){
      return false
   }
   if(tex >= 0){
      _install_skybox_tex(tex, visible)
      _build_skybox_generated_envs(env_w, env_h)
      if(verbose){
         terminal.log("skybox loaded: tex=" + to_str(tex))
      }
      return true
   } else {
      skybox_tex_id = -1
      skybox_spec_tex_id = -1
      skybox_enabled = false
      if(verbose){
         terminal.log("skybox load failed: " + path)
      }
   }
   false
}

fn _load_fast_generated_skybox(visible=true) {
   if(skybox_tex_id >= 0){
      return true
   }
   def env_w, env_h = ui_profile.generated_env_width(false, false), max(1, env_w / 2)
   mut im = compare_visible_env_im_hold && is_dict(compare_visible_env_im_hold) ? compare_visible_env_im_hold : 0
   def own_im = im ? false : true
   if(!im){
      im = generate_compare_visible_env_image(env_w, env_h)
   }
   def tex = render_scene.upload_generated_texture(im)
   if(tex < 0){
      if(own_im && im && is_dict(im)){
         img.free(im)
      }
      return false
   }
   skybox_tex_id = tex
   skybox_spec_tex_id = tex
   skybox_enabled = visible ? true : false
   if(skybox_im_hold && is_dict(skybox_im_hold)){
      img.free(skybox_im_hold)
   }
   if(_env_keep_cpu_images()){
      skybox_im_hold = own_im ? im : 0
   } else {
      if(own_im && im && is_dict(im)){ img.free(im) }
      skybox_im_hold = 0
   }
   true
}
