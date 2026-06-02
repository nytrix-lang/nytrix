;; Keywords: platform window backend opengl wgl
;; WGL context creation for the Win32 window backend.
module std.os.ui.window.platform.opengl.wgl(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address, choose_pixel_format)
use std.core
use std.core.mem (cstr)

fn ChoosePixelFormat(any: _dc, any: _pfd): int { 0 }
fn SetPixelFormat(any: _dc, any: _pf, any: _pfd): int { 0 }
fn wglCreateContext(any: _dc): any { 0 }
fn wglDeleteContext(any: _ctx): int { 0 }
fn wglMakeCurrent(any: _dc, any: _ctx): int { 0 }
fn SwapBuffers(any: _dc): int { 0 }
fn wglGetProcAddress(any: _name): any { 0 }

fn choose_pixel_format(any: dc): int {
   if(!dc){ return 0 }
   def pfd = zalloc(40)
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
   if(pf != 0){ SetPixelFormat(dc, pf, pfd) }
   free(pfd)
   pf
}

fn create_context(any: dc): any {
   if(!dc){ return 0 }
   wglCreateContext(dc)
}

fn destroy_context(any: ctx): bool {
   if(!ctx){ return false }
   wglDeleteContext(ctx)
   true
}

fn make_current(any: dc, any: ctx): bool { wglMakeCurrent(dc, ctx) != 0 }

fn swap_buffers(any: dc): bool {
   if(!dc){ return false }
   SwapBuffers(dc) != 0
}

fn swap_interval(any: interval): bool {
   false
}

fn get_proc_address(any: name): any {
   def proc_name = to_str(name)
   if(!startswith(proc_name, "gl") && !startswith(proc_name, "wgl")){ return 0 }
   def s, p = cstr(proc_name), wglGetProcAddress(s)
   if(p){ return p }
   0
}
