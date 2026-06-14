;; Keywords: viewer icons ui glyph toolbar os render
;; Icon glyph names and compact drawing helpers for viewer and editor controls.
;; References:
;; - std.os.ui.render.viewer.gui
module std.os.ui.render.viewer.icons(icon_tex, icon_sprite, warm_sprites, stable_texture_id, clear_cache)
use std.core
use std.core.str as str
use std.os.fs as osfs
use std.os.ui.render.dump as ui_profile
use std.os.ui.render as render
use std.os.ui.render.atlas as atlas
use std.os.ui.assets.viewer as assets
use std.parse.img.svg as svg

mut _cache = dict(32)
mut _sprite_cache = dict(64)
mut _atlas = 0

fn clear_cache() int {
   "Clears cached icon texture ids."
   _cache = dict(32)
   _sprite_cache = dict(64)
   _atlas = 0
   0
}

fn stable_texture_id(candidate) int {
   "Normalizes a newly-uploaded texture id for renderer backends."
   mut tex_id = int(candidate)
   def stable = int(render.texture_last_created_id())
   if stable >= 0 && stable < 1024 { return stable }
   def count = int(render.texture_count())
   if (tex_id < 0 || tex_id >= 1024) && count > 0 {
      def latest = count - 1
      if latest >= 0 && latest < 1024 { return latest }
   }
   tex_id
}

fn icon_tex(name) int {
   "Loads or returns the cached texture id for a named SVG icon."
   def trace = ui_profile.env_truthy_cached("NY_GUI_ICON_TRACE")
   def key = str.lower(str.strip(to_str(name)))
   if key.len == 0 { return -1 }
   if _cache.contains(key) {
      def cached = int(_cache.get(key, -1))
      if cached >= 0 { return cached }
      if cached == -2 { return -1 }
   }
   def path = assets.icon_path(key)
   if !osfs.is_file(path) {
      if trace { ui_profile.print_text("[gui:icon] missing key=" + key + " path=" + path) }
      _cache[key] = -2
      return -1
   }
   mut tex = -1
   def im = svg.load_path(path)
   if im && is_dict(im) {
      tex = render.texture_upload_image_ex(im, path, 37, false, false, 1, 33071, 33071, "gui_icon:" + key, true)
      tex = stable_texture_id(tex)
   } elif trace {
      ui_profile.print_text("[gui:icon] svg-decode-failed key=" + key + " path=" + path + " err=" + svg.last_error())
   }
   if tex > 0 {
      _cache[key] = tex
      if trace { ui_profile.print_text("[gui:icon] loaded key=" + key + " tex=" + to_str(tex) + " path=" + path) }
   } elif trace {
      ui_profile.print_text("[gui:icon] load-failed key=" + key + " path=" + path)
   }
   tex
}

fn _icon_atlas() any {
   if is_dict(_atlas) { return _atlas }
   atlas.atlas_set_backend(render.get_active_backend_name())
   _atlas = atlas.atlas_create(1024, 1024, render.FONT_FILTER_LINEAR, true)
   _atlas
}

fn _sprite_live(any spr) any {
   if !is_dict(spr) { return 0 }
   def a = _icon_atlas()
   if !is_dict(a) { return spr }
   mut tex = int(atlas.atlas_texture_id(a))
   if tex < 0 {
      atlas.atlas_flush(a)
      tex = int(atlas.atlas_texture_id(a))
   }
   if tex >= 0 { spr["tex"] = tex }
   spr
}

fn _sprite_record(int tex, any uv, str key) dict {
   mut dict spr = dict(3)
   spr["tex"] = tex
   spr["uv"] = uv
   spr["name"] = key
   spr
}

fn icon_sprite(name) any {
   "Loads or returns an atlas sprite for a named SVG icon."
   def trace = ui_profile.env_truthy_cached("NY_GUI_ICON_TRACE")
   def key = str.lower(str.strip(to_str(name)))
   if key.len == 0 { return -1 }
   if _sprite_cache.contains(key) {
      return _sprite_live(_sprite_cache.get(key, 0))
   }
   def path = assets.icon_path(key)
   if !osfs.is_file(path) {
      if trace { ui_profile.print_text("[gui:icon] missing sprite key=" + key + " path=" + path) }
      _sprite_cache[key] = -1
      return -1
   }
   def im = svg.load_path(path)
   if !im || !is_dict(im) {
      if trace { ui_profile.print_text("[gui:icon] svg-sprite-decode-failed key=" + key + " path=" + path + " err=" + svg.last_error()) }
      _sprite_cache[key] = -1
      return -1
   }
   def w, h = int(im.get("width", 0)), int(im.get("height", 0))
   def data = im.get("data", 0)
   def a = _icon_atlas()
   def uv = is_dict(a) ? atlas.atlas_add(a, key, w, h, data) : 0
   if !is_list(uv) || uv.len < 4 {
      if trace { ui_profile.print_text("[gui:icon] sprite-pack-failed key=" + key + " size=" + to_str(w) + "x" + to_str(h)) }
      _sprite_cache[key] = -1
      return -1
   }
   atlas.atlas_flush(a)
   def spr = _sprite_record(int(atlas.atlas_texture_id(a)), uv, key)
   _sprite_cache[key] = spr
   _sprite_live(spr)
}

fn warm_sprites(any names) int {
   "Loads a batch of SVG icon sprites and flushes the atlas once."
   if !is_list(names) { return 0 }
   def trace = ui_profile.env_truthy_cached("NY_GUI_ICON_TRACE")
   def a = _icon_atlas()
   if !is_dict(a) { return 0 }
   mut warmed = []
   mut count = 0
   mut i = 0
   while i < names.len {
      def key = str.lower(str.strip(to_str(names.get(i, ""))))
      if key.len > 0 {
         if _sprite_cache.contains(key) {
            if is_dict(_sprite_cache.get(key, 0)) { warmed = warmed.append(key) }
         } else {
            def path = assets.icon_path(key)
            if !osfs.is_file(path) {
               if trace { ui_profile.print_text("[gui:icon] missing sprite key=" + key + " path=" + path) }
               _sprite_cache[key] = -1
            } else {
               def im = svg.load_path(path)
               if !im || !is_dict(im) {
                  if trace { ui_profile.print_text("[gui:icon] svg-sprite-decode-failed key=" + key + " path=" + path + " err=" + svg.last_error()) }
                  _sprite_cache[key] = -1
               } else {
                  def w, h = int(im.get("width", 0)), int(im.get("height", 0))
                  def uv = atlas.atlas_add(a, key, w, h, im.get("data", 0))
                  if !is_list(uv) || uv.len < 4 {
                     if trace { ui_profile.print_text("[gui:icon] sprite-pack-failed key=" + key + " size=" + to_str(w) + "x" + to_str(h)) }
                     _sprite_cache[key] = -1
                  } else {
                     _sprite_cache[key] = _sprite_record(-1, uv, key)
                     warmed = warmed.append(key)
                     count += 1
                  }
               }
            }
         }
      }
      i += 1
   }
   atlas.atlas_flush(a)
   def tex = int(atlas.atlas_texture_id(a))
   if tex >= 0 {
      i = 0
      while i < warmed.len {
         def key = to_str(warmed.get(i, ""))
         def spr = _sprite_cache.get(key, 0)
         if is_dict(spr) {
            spr["tex"] = tex
            _sprite_cache[key] = spr
         }
         i += 1
      }
   }
   count
}

#main {
   assert(clear_cache() == 0 && icon_tex("") == -1, "viewer icons basics")
   print("✓ std.os.ui.render.viewer.icons self-test passed")
}
