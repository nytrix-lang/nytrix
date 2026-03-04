;; Keywords: ui window backend opengl context
;; Shared Native OpenGL context loading abstraction.

module std.ui.window.platform.opengl (
   get_proc_address,
   create_context, create_offscreen_context, destroy_offscreen_context,
   make_context_current, release_context_current, get_current_context,
   create_osmesa_context, destroy_osmesa_context,
   swap_buffers, swap_interval
)

use std.core *
use std.os.prim as prim

use std.ui.window.platform.opengl.glx as glx
use std.ui.window.platform.opengl.egl as egl
use std.ui.window.platform.opengl.wgl as wgl
use std.ui.window.platform.opengl.nsgl as nsgl

if(comptime{ __os_name() == "windows" }){
   #include <windows.h>
}

fn _get_backend_name(){
   def r = prim.env("NY_UI_BACKEND")
   if(r && len(r) > 0){ return r }
   if(comptime{ __os_name() == "linux" }){
      ;; We cannot easily check for Wayland socket without std.os.sys.
      ;; But for OpenGL purposes, we usually assume X11 if not specified or fallback to EGL.
      return "x11"
   } elif(comptime{ __os_name() == "windows" }){ return "win32" }
   elif(comptime{ __os_name() == "macos" }){ return "cocoa" }
   else { "none" }
}

fn get_proc_address(name){
   "Returns the address of the specified OpenGL core or extension function."
   def b = _get_backend_name()

   if(comptime{ __os_name() == "linux" }){
      if(b == "x11"){
         def addr = glx.get_proc_address(name)
         if(addr){ return addr }
         return egl.get_proc_address(name)
      } elif(b == "wayland"){
         return egl.get_proc_address(name)
      }
   } elif(comptime{ __os_name() == "windows" }){
      if(b == "win32"){ return wgl.get_proc_address(name) }
   } elif(comptime{ __os_name() == "macos" }){
      if(b == "cocoa"){ return nsgl.get_proc_address(name) }
   }
   0
}

fn create_offscreen_context(width=1, height=1, share_context=0){
   "Creates an offscreen OpenGL context. On Linux tries EGL/GLX; falls back to OSMesa."
   def b = _get_backend_name()
   if(comptime{ __os_name() == "linux" }){
      if(b == "wayland"){
         def dpy = egl.init_display(0)
         if(!dpy){ return create_osmesa_context(width, height) }
         def attrs = [0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 16, 0x3038]
         def config = egl.choose_config(dpy, attrs)
         if(!config){ return create_osmesa_context(width, height) }
         def ctx = egl.create_context(dpy, config, share_context)
         if(!ctx){ return create_osmesa_context(width, height) }
         mut out = dict()
         out = dict_set(out, "type", "egl_offscreen")
         out = dict_set(out, "display", dpy)
         out = dict_set(out, "config", config)
         out = dict_set(out, "context", ctx)
         return out
      } elif(b == "x11"){
         def ctx = glx.create_context(0, 0, share_context)
         if(!ctx){ return create_osmesa_context(width, height) }
         mut out = dict()
         out = dict_set(out, "type", "glx_offscreen")
         out = dict_set(out, "context", ctx)
         return out
      }
      return create_osmesa_context(width, height)
   } elif(comptime{ __os_name() == "windows" }){
      if(b == "win32"){
         ;; WGL: create hidden dummy window to get HDC
         def cls_name = malloc(14)
         if(!cls_name){ return 0 }
         memset(cls_name, 0, 14)
         store16(cls_name, 83, 0) ;; S
         store16(cls_name, 84, 2) ;; T
         store16(cls_name, 65, 4) ;; A
         store16(cls_name, 84, 6) ;; T
         store16(cls_name, 73, 8) ;; I
         store16(cls_name, 67, 10) ;; C
         def hwnd = CreateWindowExW(0, cls_name, cls_name, 0x80000000, 0, 0, 1, 1, 0, 0, 0, 0)
         free(cls_name)
         if(!hwnd){ return 0 }
         def dc = GetDC(hwnd)
         if(!dc){
         DestroyWindow(hwnd)
         return 0
         }
         wgl.choose_pixel_format(dc)
         def ctx = wgl.create_context(dc)
         if(!ctx){
         ReleaseDC(hwnd, dc)
         DestroyWindow(hwnd)
         return 0
         }
         if(share_context){ wglShareLists(share_context, ctx) }
         mut out = dict()
         out = dict_set(out, "type", "wgl_offscreen")
         out = dict_set(out, "hwnd", hwnd)
         out = dict_set(out, "dc", dc)
         out = dict_set(out, "context", ctx)
         return out
      }
      return 0
   } else {
      if(b == "cocoa"){
         def ctx = nsgl.create_context(share_context)
         if(!ctx){ return 0 }
         mut out = dict()
         out = dict_set(out, "type", "nsgl_offscreen")
         out = dict_set(out, "context", ctx)
         return out
      }
      return 0
   }
}

fn destroy_offscreen_context(ctx){
   "Destroys an offscreen OpenGL context."
   if(!ctx || !is_dict(ctx)){ return false }
   def typ = dict_get(ctx, "type", "")
   if(typ == "egl_offscreen"){
      def dpy = dict_get(ctx, "display", 0)
      def gl_ctx = dict_get(ctx, "context", 0)
      if(gl_ctx && dpy){ egl.destroy_context(dpy, gl_ctx) }
      return true
   }
   if(typ == "glx_offscreen"){
      def gl_ctx = dict_get(ctx, "context", 0)
      if(gl_ctx){ glx.destroy_context(0, gl_ctx) }
      return true
   }
   if(typ == "wgl_offscreen"){
      def gl_ctx = dict_get(ctx, "context", 0)
      def dc = dict_get(ctx, "dc", 0)
      def hwnd = dict_get(ctx, "hwnd", 0)
      if(gl_ctx){ wgl.destroy_context(gl_ctx) }
      if(dc && hwnd){ ReleaseDC(hwnd, dc) }
      if(hwnd){ DestroyWindow(hwnd) }
      return true
   }
   if(typ == "nsgl_offscreen"){
      def gl_ctx = dict_get(ctx, "context", 0)
      if(gl_ctx){ nsgl.destroy_context(gl_ctx) }
      return true
   }
   if(typ == "osmesa"){
      def gl_ctx = dict_get(ctx, "context", 0)
      def buf = dict_get(ctx, "buffer", 0)
      if(gl_ctx){
         if(comptime{ __os_name() == "macos" }){
         OSMesaDestroyContext(gl_ctx)
         }
      }
      if(buf){ free(buf) }
      return true
   }
   false
}

mut _current_context = 0

fn make_context_current(ctx){
   "Makes the given context current on the calling thread."
   if(!ctx || !is_dict(ctx)){ return false }
   _current_context = ctx
   def typ = dict_get(ctx, "type", "")
   if(typ == "egl_offscreen"){
      def dpy = dict_get(ctx, "display", 0)
      def gl_ctx = dict_get(ctx, "context", 0)
      if(dpy && gl_ctx){
         return egl.make_current(dpy, 0, 0, gl_ctx)
      }
   }
   if(typ == "glx_offscreen"){
      def gl_ctx = dict_get(ctx, "context", 0)
      if(gl_ctx){
         return glx.make_current(0, 0, gl_ctx)
      }
   }
   if(typ == "wgl_offscreen"){
      def dc = dict_get(ctx, "dc", 0)
      def gl_ctx = dict_get(ctx, "context", 0)
      if(dc && gl_ctx){ return wgl.make_current(dc, gl_ctx) }
   }
   if(typ == "nsgl_offscreen"){
      def gl_ctx = dict_get(ctx, "context", 0)
      if(gl_ctx){ return nsgl.make_current(gl_ctx) }
   }
   if(typ == "osmesa"){
      def gl_ctx = dict_get(ctx, "context", 0)
      def buf = dict_get(ctx, "buffer", 0)
      if(gl_ctx && buf){
         if(comptime{ __os_name() == "macos" }){
         def w = dict_get(ctx, "width", 1)
         def h = dict_get(ctx, "height", 1)
         if(OSMesaMakeCurrent(gl_ctx, buf, 0x1401, w, h)){ ;; GL_UNSIGNED_BYTE = 0x1401
               _current_context = ctx
               return true
         }
         }
      }
   }
   false
}

fn release_context_current(){
   "Releases the current OpenGL context on the calling thread."
   _current_context = 0
   def b = _get_backend_name()
   if(comptime{ __os_name() == "linux" }){
      if(b == "wayland"){ return egl.make_current(0, 0, 0, 0) }
      if(b == "x11"){ return glx.make_current(0, 0, 0) }
   } elif(comptime{ __os_name() == "windows" }){
      if(b == "win32"){ return wgl.make_current(0, 0) }
   } elif(comptime{ __os_name() == "macos" }){
      if(b == "cocoa"){ return nsgl.make_current(0) }
   }
   true
}

fn get_current_context(){
   "Returns the context most recently passed to make_context_current."
   _current_context
}

if(comptime{ __os_name() == "macos" }){
   #include <GL/osmesa.h>
}

fn create_osmesa_context(width=1, height=1){
   "Creates an OSMesa software-rendered offscreen context. Returns 0 if OSMesa is unavailable."
   if(comptime{ __os_name() == "macos" }){
      ;; OSMESA_RGBA = 0x1908
      def ctx = OSMesaCreateContext(0x1908, 0)
      if(!ctx){ return 0 }
      def buf = malloc(width * height * 4)
      if(!buf){
         OSMesaDestroyContext(ctx)
         return 0
      }
      memset(buf, 0, width * height * 4)
      mut out = dict()
      out = dict_set(out, "type", "osmesa")
      out = dict_set(out, "context", ctx)
      out = dict_set(out, "buffer", buf)
      out = dict_set(out, "width", width)
      out = dict_set(out, "height", height)
      return out
   }
   0
}

fn destroy_offscreen_context(ctx){
   if(!ctx || !is_dict(ctx)){ return false }
   def typ = dict_get(ctx, "type", "")
   if(typ == "osmesa"){
      def gl_ctx = dict_get(ctx, "context", 0)
      def buf = dict_get(ctx, "buffer", 0)
      if(gl_ctx){
         if(comptime{ __os_name() == "macos" }){
         OSMesaDestroyContext(gl_ctx)
         }
      }
      if(buf){ free(buf) }
      return true
   }
   false
}

fn swap_buffers(ctx){
   if(!ctx || !is_dict(ctx)){ return false }
   def typ = dict_get(ctx, "type", "")
   if(typ == "egl_offscreen"){
      def dpy = dict_get(ctx, "display", 0)
      def surf = dict_get(ctx, "surface", 0)
      if(dpy && surf){ return egl.swap_buffers(dpy, surf) }
   }
   if(typ == "glx_offscreen"){
      ;; Placeholder for GLX swap
   }
   false
}

fn swap_interval(interval){
   def b = _get_backend_name()
   if(comptime{ __os_name() == "linux" }){
      if(b == "wayland"){ return egl.swap_interval(0, interval) }
      if(b == "x11"){ return glx.swap_interval(interval) }
   }
   false
}

fn destroy_osmesa_context(ctx){
   "Destroys an OSMesa context."
   destroy_offscreen_context(ctx)
}

fn create_context(native, hints){
   "Creates an OpenGL context for a native window."
   def b = _get_backend_name()
   if(comptime{ __os_name() == "linux" }){
      if(b == "wayland"){
         def g = dict_get(native, "globals", 0)
         def d = dict_get(g, "display", 0)
         def w = dict_get(native, "handle", 0)
         def dpy = egl.init_display(d)
         if(!dpy){ return 0 }
         def attrs = [0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3038]
         def config = egl.choose_config(dpy, attrs)
         if(!config){ return 0 }
         def surface = egl.create_surface(dpy, config, w)
         if(!surface){ return 0 }
         def ctx = egl.create_context(dpy, config, 0)
         if(!ctx){ return 0 }
         mut out = dict()
         out = dict_set(out, "type", "egl")
         out = dict_set(out, "display", dpy)
         out = dict_set(out, "surface", surface)
         out = dict_set(out, "context", ctx)
         return out
      }
      if(b == "x11"){
         def d = dict_get(native, "display", 0)
         def w = dict_get(native, "handle", 0)
         def ctx = glx.create_context(d, w, 0)
         if(!ctx){ return 0 }
         mut out = dict()
         out = dict_set(out, "type", "glx")
         out = dict_set(out, "context", ctx)
         return out
      }
   }
   0
}
