;; Keywords: platform window backend opengl nsgl glx egl
;; OpenGL context facade for EGL, GLX, and NSGL platform backends.
module std.os.ui.window.platform.opengl(get_proc_address, create_context, create_offscreen_context, destroy_offscreen_context, make_context_current, release_context_current, get_current_context, create_osmesa_context, destroy_osmesa_context, swap_buffers, swap_interval)
use std.core
use std.core.common as common
use std.os.ui.window.platform.api as api
use std.os.ui.window.platform.opengl.glx as glx
use std.os.ui.window.platform.opengl.egl as egl
use std.os.ui.window.platform.opengl.nsgl as nsgl
#macos {
   fn OSMesaCreateContext(any: _format, any: _share): any { 0 }
   fn OSMesaDestroyContext(any: _ctx): any { 0 }
   fn OSMesaMakeCurrent(any: _ctx, any: _buffer, any: _type, any: _w, any: _h): int { 0 }
} #endif
mut _current_context = 0

fn _get_backend_name(): str {
   def requested = common.env_lower("NY_UI_BACKEND")
   if(requested.len > 0){ return requested }
   #linux { return "x11" }
   #elif windows { return "win32" }
   #elif macos { return "cocoa" }
   #endif
   "none"
}

fn get_proc_address(any: name): any {
   "Returns the address of the specified OpenGL core or extension function."
   def proc_name = to_str(name)
   if(!startswith(proc_name, "gl") && !startswith(proc_name, "egl")){ return 0 }
   def b = _get_backend_name()
   #linux {
      if(b == "x11"){
         def addr = glx.get_proc_address(proc_name)
         if(addr){ return addr }
         return egl.get_proc_address(proc_name)
      }
      if(b == "wayland"){ return egl.get_proc_address(proc_name) }
   } #elif windows {
      return 0
   } #elif macos {
      if(b == "cocoa"){ return nsgl.get_proc_address(proc_name) }
   } #endif
   0
}

fn create_offscreen_context(int: width=1, int: height=1, any: share_context=0): any {
   "Creates an offscreen OpenGL context. On Linux tries EGL/GLX; falls back to OSMesa."
   def b = _get_backend_name()
   #linux {
      if(b == "wayland"){
         def dpy = egl.init_display(0)
         if(!dpy){ return create_osmesa_context(width, height) }
         def attrs = [0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 16, 0x3038]
         def config = egl.choose_config(dpy, attrs)
         if(!config){ return create_osmesa_context(width, height) }
         def ctx = egl.create_context(dpy, config, share_context)
         if(!ctx){ return create_osmesa_context(width, height) }
         return {"type": "egl_offscreen", "display": dpy, "config": config, "context": ctx}
      }
      if(b == "x11"){
         def ctx = glx.create_context(0, 0, share_context)
         if(!ctx){ return create_osmesa_context(width, height) }
         return {"type": "glx_offscreen", "context": ctx}
      }
      return create_osmesa_context(width, height)
   } #elif windows {
      return 0
   } #elif macos {
      if(b == "cocoa"){
         def ctx = nsgl.create_context(share_context)
         if(!ctx){ return 0 }
         return {"type": "nsgl_offscreen", "context": ctx}
      }
      return 0
   } #endif
   0
}

fn destroy_offscreen_context(any: ctx): bool {
   "Destroys an offscreen OpenGL context."
   if(!ctx || !is_dict(ctx)){ return false }
   def typ = ctx.get("type", "")
   if(typ == "egl_offscreen"){
      def dpy = ctx.get("display", 0)
      def gl_ctx = ctx.get("context", 0)
      if(gl_ctx && dpy){ egl.destroy_context(dpy, gl_ctx) }
      return true
   }
   if(typ == "glx_offscreen"){
      def gl_ctx = ctx.get("context", 0)
      if(gl_ctx){ glx.destroy_context(0, gl_ctx) }
      return true
   }
   if(typ == "nsgl_offscreen"){
      def gl_ctx = ctx.get("context", 0)
      if(gl_ctx){ nsgl.destroy_context(gl_ctx) }
      return true
   }
   if(typ == "osmesa"){
      def gl_ctx = ctx.get("context", 0)
      def buf = ctx.get("buffer", 0)
      #macos {
         if(gl_ctx){ OSMesaDestroyContext(gl_ctx) }
      } #endif
      if(buf){ free(buf) }
      return true
   }
   false
}

fn make_context_current(any: ctx): bool {
   "Makes the given context current on the calling thread."
   if(!ctx || !is_dict(ctx)){ return false }
   _current_context = ctx
   def typ = ctx.get("type", "")
   if(typ == "egl_offscreen"){
      def dpy = ctx.get("display", 0)
      def gl_ctx = ctx.get("context", 0)
      if(dpy && gl_ctx){ return egl.make_current(dpy, 0, 0, gl_ctx) }
   }
   if(typ == "glx_offscreen"){
      def gl_ctx = ctx.get("context", 0)
      if(gl_ctx){ return glx.make_current(0, 0, gl_ctx) }
   }
   if(typ == "nsgl_offscreen"){
      def gl_ctx = ctx.get("context", 0)
      if(gl_ctx){ return nsgl.make_current(gl_ctx) }
   }
   #macos {
      if(typ == "osmesa"){
         def gl_ctx = ctx.get("context", 0)
         def buf = ctx.get("buffer", 0)
         if(gl_ctx && buf){
            def w, h = ctx.get("width", 1), ctx.get("height", 1)
            if(OSMesaMakeCurrent(gl_ctx, buf, 0x1401, w, h)){
               _current_context = ctx
               return true
            }
         }
      }
   } #endif
   false
}

fn release_context_current(): bool {
   "Releases the current OpenGL context on the calling thread."
   _current_context = 0
   def b = _get_backend_name()
   #linux {
      if(b == "wayland"){ return egl.make_current(0, 0, 0, 0) }
      if(b == "x11"){ return glx.make_current(0, 0, 0) }
   } #elif windows {
      return true
   } #elif macos {
      if(b == "cocoa"){ return nsgl.make_current(0) }
   } #endif
   true
}

fn get_current_context(): any {
   "Returns the context most recently passed to make_context_current."
   _current_context
}

fn create_osmesa_context(int: width=1, int: height=1): any {
   "Creates an OSMesa software-rendered offscreen context. Returns 0 if OSMesa is unavailable."
   #macos {
      def ctx = OSMesaCreateContext(0x1908, 0)
      if(!ctx){ return 0 }
      def buf = malloc(width * height * 4)
      if(!buf){
         OSMesaDestroyContext(ctx)
         return 0
      }
      memset(buf, 0, width * height * 4)
      return {"type": "osmesa", "context": ctx, "buffer": buf, "width": width, "height": height}
   } #endif
   0
}

fn destroy_osmesa_context(any: ctx): bool {
   "Destroys an OSMesa context."
   destroy_offscreen_context(ctx)
}

fn swap_buffers(any: ctx): bool {
   if(!ctx || !is_dict(ctx)){ return false }
   def typ = ctx.get("type", "")
   if(typ == "egl_offscreen"){
      def dpy = ctx.get("display", 0)
      def surf = ctx.get("surface", 0)
      if(dpy && surf){ return egl.swap_buffers(dpy, surf) }
   }
   if(typ == "glx_offscreen"){ return false }
   false
}

fn swap_interval(any: interval): bool {
   if(!_current_context){ return false }
   def b = _get_backend_name()
   #linux {
      if(b == "wayland"){ return egl.swap_interval(0, interval) }
      if(b == "x11"){ return glx.swap_interval(interval) }
   } #endif
   false
}

fn choose_visual(any: hints, any: display=0, any: screen=0): list {
   "Returns [visual, depth] matched to the given hints."
   def b = _get_backend_name()
   #linux {
      if(b == "x11"){
         def attrs = [
            0x8002, 1,
            0x8010, 0x01,
            0x8011, 0x01,
            0x0008, hints.get(api.RED_BITS, 8),
            0x0009, hints.get(api.GREEN_BITS, 8),
            0x000a, hints.get(api.BLUE_BITS, 8),
            0x000b, hints.get(api.ALPHA_BITS, 8),
            0x000c, hints.get(api.DEPTH_BITS, 24),
            0x000d, hints.get(api.STENCIL_BITS, 8),
            0x0005, 1
         ]
         def config = glx.choose_fb_config(display, screen, attrs)
         if(!config){ return [0, 0] }
         def visual = glx.get_visual(display, config)
         def depth = glx.get_fbconfig_attrib(display, config, 2)
         return [visual, depth]
      }
   } #endif
   [0, 0]
}

fn create_context(any: native, any: hints): any {
   "Create an OpenGL context for a native window using backend context hints."
   def b = _get_backend_name()
   #linux {
      if(b == "wayland"){
         def g, d = native.get("globals", 0), g.get("display", 0)
         def w = native.get("handle", 0)
         def dpy = egl.init_display(d)
         if(!dpy){ return 0 }
         def attrs = [
            0x3024, hints.get(api.RED_BITS, 8),
            0x3023, hints.get(api.GREEN_BITS, 8),
            0x3022, hints.get(api.BLUE_BITS, 8),
            0x3021, hints.get(api.ALPHA_BITS, 8),
            0x3025, hints.get(api.DEPTH_BITS, 24),
            0x3038
         ]
         def config = egl.choose_config(dpy, attrs)
         if(!config){ return 0 }
         def surface = egl.create_surface(dpy, config, w)
         if(!surface){ return 0 }
         def ctx = egl.create_context(dpy, config, 0)
         if(!ctx){ return 0 }
         return {"type": "egl", "display": dpy, "surface": surface, "context": ctx}
      }
      if(b == "x11"){
         def d, w = native.get("display", 0), native.get("handle", 0)
         def s = native.get("screen", 0)
         def attrs = [
            0x8002, 1,
            0x8010, 0x01,
            0x8011, 0x01,
            0x0008, hints.get(api.RED_BITS, 8),
            0x0009, hints.get(api.GREEN_BITS, 8),
            0x000a, hints.get(api.BLUE_BITS, 8),
            0x000b, hints.get(api.ALPHA_BITS, 8),
            0x000c, hints.get(api.DEPTH_BITS, 24),
            0x000d, hints.get(api.STENCIL_BITS, 8),
            0x0005, 1
         ]
         def fbconfig = glx.choose_fb_config(d, s, attrs)
         if(!fbconfig){ return 0 }
         def ctx = glx.create_context(d, fbconfig, 0, true)
         if(!ctx){ return 0 }
         return {"type": "glx", "display": d, "window": w, "context": ctx}
      }
   } #endif
   0
}
