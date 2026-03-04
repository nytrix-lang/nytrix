;; Keywords: ui window backend wgl
;; WGL Context helper for Win32 Nytrix backend.

module std.ui.window.platform.opengl.wgl (
   create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address,
   choose_pixel_format
)

use std.core *

if(comptime{ __os_name() == "windows" }){
   #include <windows.h>
   #include <GL/gl.h>
}

fn choose_pixel_format(dc){
   if(!dc){ return 0 }
   def pfd = calloc(1, 40)
   if(!pfd){ return 0 }
   store16(pfd, 40, 0) ;; nSize
   store16(pfd, 1, 2) ;; nVersion
   store32(pfd, 0x00000004 | 0x00000020 | 0x00000001, 4) ;; PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER
   store8(pfd, 0, 8) ;; iPixelType (PFD_TYPE_RGBA)
   store8(pfd, 32, 9) ;; cColorBits
   store8(pfd, 24, 23) ;; cDepthBits
   store8(pfd, 8, 24) ;; cStencilBits
   store8(pfd, 0, 25) ;; iLayerType (PFD_MAIN_PLANE)

   def pf = ChoosePixelFormat(dc, pfd)
   if(pf != 0){
      SetPixelFormat(dc, pf, pfd)
   }
   free(pfd)
   pf
}

fn create_context(dc){
   if(!dc){ return 0 }
   wglCreateContext(dc)
}

fn destroy_context(ctx){
   if(!ctx){ return false }
   wglDeleteContext(ctx)
   true
}

fn make_current(dc, ctx){
   wglMakeCurrent(dc, ctx) != 0
}

fn swap_buffers(dc){
   if(!dc){ return false }
   SwapBuffers(dc) != 0
}

fn swap_interval(interval){
   ;; Requires wglSwapIntervalEXT from WGL_EXT_swap_control
   false
}

fn get_proc_address(name){
   def s = cstr(name)
   def p = wglGetProcAddress(s)
   if(p){ return p }
   0
}
