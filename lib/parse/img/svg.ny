;; Keywords: image svg vector
;; Native SVG image loader for Nytrix using librsvg + cairo.
module std.parse.img.svg(decode, load_path, available, last_error, backend_name)
use std.core
use std.core.mem
use std.core.dict_mod as dict_mod
use std.os (env, file_exists)
use std.os.ffi (dlopen_checked, dlsym, RTLD_NOW, RTLD_GLOBAL, call1, call1_void, call2, call2_void, call3)
use std.core.str as str
use std.math (abs)

if(comptime{ __os_name() == "linux" || __os_name() == "macos" }){
   #include <librsvg-2.0/librsvg/rsvg.h> as "rsvg_"
   #include <cairo/cairo.h> as "cairo_"
}

#windows {
   fn rsvg_handle_new_from_data(..._args): any { 0 }
   fn rsvg_handle_new_from_file(..._args): any { 0 }
   fn rsvg_handle_get_dimensions(..._args): any { 0 }
   fn rsvg_handle_render_cairo(..._args): int { 0 }
   fn cairo_image_surface_create(..._args): any { 0 }
   fn cairo_image_surface_get_stride(any: _surface): int { 0 }
   fn cairo_image_surface_get_data(any: _surface): any { 0 }
   fn cairo_surface_flush(any: _surface): any { 0 }
   fn cairo_surface_destroy(any: _surface): any { 0 }
   fn cairo_create(any: _surface): any { 0 }
   fn cairo_destroy(any: _cr): any { 0 }
   fn cairo_set_operator(any: _cr, any: _op): any { 0 }
   fn cairo_paint(any: _cr): any { 0 }
   fn cairo_scale(any: _cr, any: _sx, any: _sy): any { 0 }
} #endif

def _CAIRO_FORMAT_ARGB32 = 0
def _CAIRO_OPERATOR_CLEAR = 0
def _CAIRO_OPERATOR_OVER = 2
def _RSVG_DIMENSION_DATA_SIZE = 24
mut _svg_checked = false
mut _svg_ok = false
mut _svg_last_error = ""
mut _svg_backend_name = ""
mut _svg_lib_rsvg = 0
mut _svg_lib_cairo = 0
mut _svg_lib_glib = 0
mut _svg_lib_gobject = 0
mut _ptr_rsvg_handle_new_from_data = 0
mut _ptr_rsvg_handle_new_from_file = 0
mut _ptr_rsvg_handle_get_dimensions = 0
mut _ptr_rsvg_handle_render_cairo = 0
mut _ptr_cairo_image_surface_create = 0
mut _ptr_cairo_image_surface_get_stride = 0
mut _ptr_cairo_image_surface_get_data = 0
mut _ptr_cairo_surface_flush = 0
mut _ptr_cairo_surface_destroy = 0
mut _ptr_cairo_create = 0
mut _ptr_cairo_destroy = 0
mut _ptr_g_error_free = 0
mut _ptr_g_object_unref = 0

fn _svg_set_error(any: msg): bool {
   _svg_last_error = to_str(msg)
   false
}

fn last_error(): str {
   "Returns the last SVG backend error."
   _svg_last_error
}

fn backend_name(): str {
   "Returns the active SVG backend name, or empty string when unavailable."
   _svg_backend_name
}

fn _svg_g_error_free(any: err): int {
   if(err && _ptr_g_error_free){ call1_void(_ptr_g_error_free, err) }
   0
}

fn _svg_g_object_unref(any: obj): int {
   if(obj && _ptr_g_object_unref){ call1_void(_ptr_g_object_unref, obj) }
   0
}

fn _svg_error_message(any: err_pp, str: fallback): str {
   if(!err_pp){ return fallback }
   def err = load64_h(err_pp, 0)
   if(!err){ return fallback }
   def msg_ptr = load64_h(err, 8)
   def msg = msg_ptr ? str.cstr_to_str(msg_ptr) : fallback
   _svg_g_error_free(err)
   store64_h(err_pp, 0, 0)
   msg.len > 0 ? msg : fallback
}

fn _svg_load_dyn(): bool {
   if(_svg_checked){ return _svg_ok }
   _svg_checked = true
   _svg_ok = false
   _svg_backend_name = ""
   _svg_last_error = ""
   if(!comptime{ __os_name() == "linux" || __os_name() == "macos" }){ return _svg_set_error("native SVG backend unsupported on this OS; install a supported backend for this platform") }
   mut rsvg = 0
   mut cairo = 0
   mut glib = 0
   mut gobj = 0
   if(comptime{ __os_name() == "linux" }){
      rsvg = dlopen_checked("librsvg-2.so.2", "rsvg_handle_new_from_data", RTLD_NOW() | RTLD_GLOBAL())
      if(!rsvg){ rsvg = dlopen_checked("librsvg-2.so", "rsvg_handle_new_from_data", RTLD_NOW() | RTLD_GLOBAL()) }
      cairo = dlopen_checked("libcairo.so.2", "cairo_image_surface_create", RTLD_NOW() | RTLD_GLOBAL())
      if(!cairo){ cairo = dlopen_checked("libcairo.so", "cairo_image_surface_create", RTLD_NOW() | RTLD_GLOBAL()) }
      glib = dlopen_checked("libglib-2.0.so.0", "g_error_free", RTLD_NOW() | RTLD_GLOBAL())
      if(!glib){ glib = dlopen_checked("libglib-2.0.so", "g_error_free", RTLD_NOW() | RTLD_GLOBAL()) }
      gobj = dlopen_checked("libgobject-2.0.so.0", "g_object_unref", RTLD_NOW() | RTLD_GLOBAL())
      if(!gobj){ gobj = dlopen_checked("libgobject-2.0.so", "g_object_unref", RTLD_NOW() | RTLD_GLOBAL()) }
   } else {
      rsvg = dlopen_checked("librsvg-2.dylib", "rsvg_handle_new_from_data", RTLD_NOW() | RTLD_GLOBAL())
      if(!rsvg){ rsvg = dlopen_checked("librsvg-2", "rsvg_handle_new_from_data", RTLD_NOW() | RTLD_GLOBAL()) }
      cairo = dlopen_checked("libcairo.dylib", "cairo_image_surface_create", RTLD_NOW() | RTLD_GLOBAL())
      if(!cairo){ cairo = dlopen_checked("cairo", "cairo_image_surface_create", RTLD_NOW() | RTLD_GLOBAL()) }
      glib = dlopen_checked("libglib-2.0.dylib", "g_error_free", RTLD_NOW() | RTLD_GLOBAL())
      if(!glib){ glib = dlopen_checked("glib-2.0", "g_error_free", RTLD_NOW() | RTLD_GLOBAL()) }
      gobj = dlopen_checked("libgobject-2.0.dylib", "g_object_unref", RTLD_NOW() | RTLD_GLOBAL())
      if(!gobj){ gobj = dlopen_checked("gobject-2.0", "g_object_unref", RTLD_NOW() | RTLD_GLOBAL()) }
   }
   if(!rsvg || !cairo || !glib || !gobj){ return _svg_set_error("missing native SVG libs: need librsvg + cairo + glib + gobject") }
   _svg_lib_rsvg = rsvg
   _svg_lib_cairo = cairo
   _svg_lib_glib = glib
   _svg_lib_gobject = gobj
   _ptr_g_error_free = dlsym(glib, "g_error_free")
   _ptr_g_object_unref = dlsym(gobj, "g_object_unref")
   if(!_ptr_g_error_free || !_ptr_g_object_unref){ return _svg_set_error("native SVG backend incomplete: required librsvg/cairo/glib symbols not found") }
   _svg_backend_name = "librsvg+cairo"
   _svg_last_error = ""
   _svg_ok = true
   true
}

fn available(): bool {
   "Returns whether the native SVG backend is available."
   _svg_load_dyn()
}

fn _looks_like_svg(any: data): bool {
   if(!is_str(data) || data.len < 4){ return false }
   mut i = 0
   def limit = min(data.len - 3, 512)
   while(i < limit){
      if(load8(data, i) == 60){
         def c1, c2 = load8(data, i + 1) | 32, load8(data, i + 2) | 32
         def c3 = load8(data, i + 3) | 32
         if(c1 == 115 && c2 == 118 && c3 == 103){ return true }
      }
      i += 1
   }
   false
}

fn _svg_decode_surface(any: surface, any: svg_handle, any: out_w, any: out_h, any: scale): any {
   if(!surface){
      _svg_g_object_unref(svg_handle)
      return 0
   }
   def stride = cairo_image_surface_get_stride(surface)
   def src = cairo_image_surface_get_data(surface)
   if(!src || stride <= 0){
      cairo_surface_destroy(surface)
      _svg_g_object_unref(svg_handle)
      _svg_set_error("cairo image surface is invalid")
      return 0
   }
   def w, h = int(out_w), int(out_h)
   if(w <= 0 || h <= 0){
      cairo_surface_destroy(surface)
      _svg_g_object_unref(svg_handle)
      _svg_set_error("invalid SVG target surface size")
      return 0
   }
   def cr = cairo_create(surface)
   if(!cr){
      cairo_surface_destroy(surface)
      _svg_g_object_unref(svg_handle)
      _svg_set_error("cairo_create failed")
      return 0
   }
   cairo_set_operator(cr, _CAIRO_OPERATOR_CLEAR)
   cairo_paint(cr)
   cairo_set_operator(cr, _CAIRO_OPERATOR_OVER)
   if(abs(float(scale) - 1.0) > 0.000001){ cairo_scale(cr, float(scale), float(scale)) }
   def ok = int(rsvg_handle_render_cairo(svg_handle, cr))
   cairo_destroy(cr)
   if(ok == 0){
      cairo_surface_destroy(surface)
      _svg_g_object_unref(svg_handle)
      _svg_set_error("librsvg render failed")
      return 0
   }
   cairo_surface_flush(surface)
   def out_len = w * h * 4
   def out_ptr = malloc(out_len + 32)
   if(!out_ptr){
      cairo_surface_destroy(surface)
      _svg_g_object_unref(svg_handle)
      _svg_set_error("out of memory converting SVG image")
      return 0
   }
   mut y = 0
   while(y < h){
      def src_row = src + y * stride
      def dst_row = out_ptr + y * w * 4
      mut x = 0
      while(x < w){
         def px = src_row + x * 4
         def b = load8(px, 0)
         def g = load8(px, 1)
         def r = load8(px, 2)
         def a = load8(px, 3)
         def dst = dst_row + x * 4
         if(a <= 0){
            store8(dst, 0, 0)
            store8(dst, 0, 1)
            store8(dst, 0, 2)
            store8(dst, 0, 3)
         } elif(a >= 255){
            store8(dst, r, 0)
            store8(dst, g, 1)
            store8(dst, b, 2)
            store8(dst, a, 3)
         } else {
            def rr = min(255, int((r * 255 + (a / 2)) / a))
            def gg = min(255, int((g * 255 + (a / 2)) / a))
            def bb = min(255, int((b * 255 + (a / 2)) / a))
            store8(dst, rr, 0)
            store8(dst, gg, 1)
            store8(dst, bb, 2)
            store8(dst, a, 3)
         }
         x += 1
      }
      y += 1
   }
   cairo_surface_destroy(surface)
   _svg_g_object_unref(svg_handle)
   def rgba = init_str(out_ptr, out_len)
   mut out = dict(4)
   out = dict_mod.dict_write(out, "data", rgba)
   out = dict_mod.dict_write(out, "width", w)
   out = dict_mod.dict_write(out, "height", h)
   out = dict_mod.dict_write(out, "channels", 4)
   _svg_last_error = ""
   out
}

fn _render_handle(any: svg_handle): any {
   if(!svg_handle){ return 0 }
   def dims = malloc(_RSVG_DIMENSION_DATA_SIZE)
   if(!dims){
      _svg_g_object_unref(svg_handle)
      _svg_set_error("out of memory allocating SVG dimensions")
      return 0
   }
   memset(dims, 0, _RSVG_DIMENSION_DATA_SIZE)
   rsvg_handle_get_dimensions(svg_handle, dims)
   mut natural_w, natural_h = load32(dims, 0), load32(dims, 4)
   free(dims)
   if(natural_w <= 0 || natural_h <= 0){ natural_w, natural_h = 64, 64 }
   if(natural_w > 32768 || natural_h > 32768){
      _svg_g_object_unref(svg_handle)
      _svg_set_error("SVG dimensions are too large")
      return 0
   }
   mut target_max = 64.0
   def target_raw = env("NY_SVG_MIN_RASTER")
   if(target_raw){
      target_max = float(str.atof(to_str(target_raw)))
      if(target_max < 16.0){ target_max = 16.0 } elif(target_max > 512.0){ target_max = 512.0 }
   }
   mut scale = 1.0
   def max_dim = float(max(natural_w, natural_h))
   if(max_dim > 0.0 && max_dim < target_max){ scale = target_max / max_dim }
   mut w, h = int(float(natural_w) * scale + 0.5), int(float(natural_h) * scale + 0.5)
   if(w <= 0){ w = 1 }
   if(h <= 0){ h = 1 }
   if(w > 32768 || h > 32768){
      _svg_g_object_unref(svg_handle)
      _svg_set_error("scaled SVG dimensions are too large")
      return 0
   }
   def surface = cairo_image_surface_create(_CAIRO_FORMAT_ARGB32, w, h)
   _svg_decode_surface(surface, svg_handle, w, h, scale)
}

fn decode(any: data, any: _source_path=""): any {
   "Decodes SVG bytes through the native librsvg + cairo backend."
   if(!_looks_like_svg(data)){ return 0 }
   if(!_svg_load_dyn()){ return 0 }
   if(!is_str(data) || data.len == 0){
      _svg_set_error("empty SVG payload")
      return 0
   }
   def err_pp = malloc(8)
   if(!err_pp){
      _svg_set_error("out of memory allocating SVG error pointer")
      return 0
   }
   store64_h(err_pp, 0, 0)
   def svg_handle = rsvg_handle_new_from_data(data, data.len, err_pp)
   if(!svg_handle){
      def msg = _svg_error_message(err_pp, "librsvg failed to parse SVG data")
      free(err_pp)
      _svg_set_error(msg)
      return 0
   }
   free(err_pp)
   _render_handle(svg_handle)
}

fn load_path(any: path): any {
   "Loads and decodes an SVG file directly through librsvg, preserving file-relative references."
   if(!_svg_load_dyn()){ return 0 }
   if(!is_str(path) || path.len == 0 || !file_exists(path)){
      _svg_set_error("SVG path not found: " + to_str(path))
      return 0
   }
   def err_pp = malloc(8)
   if(!err_pp){
      _svg_set_error("out of memory allocating SVG error pointer")
      return 0
   }
   store64_h(err_pp, 0, 0)
   def svg_handle = rsvg_handle_new_from_file(cstr(path), err_pp)
   if(!svg_handle){
      def msg = _svg_error_message(err_pp, "librsvg failed to open SVG file")
      free(err_pp)
      _svg_set_error(msg)
      return 0
   }
   free(err_pp)
   _render_handle(svg_handle)
}
