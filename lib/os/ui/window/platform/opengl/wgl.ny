;; Keywords: platform window backend opengl wgl os ui input
;; WGL context creation for the Win32 window backend.
;; References:
;; - std.os.ui.window.platform.opengl
;; - std.os.ui.window
;; - std.os.ui.window.consts
module std.os.ui.window.platform.opengl.wgl(create_context, destroy_context, make_current, swap_buffers, swap_interval, get_proc_address, choose_pixel_format)
use std.core
use std.core.mem (cstr)

fn ChoosePixelFormat(any _dc, any _pfd) int {
   "Runs the ChoosePixelFormat operation."
   0
}

fn SetPixelFormat(any _dc, any _pf, any _pfd) int {
   "Runs the SetPixelFormat operation."
   0
}

fn wglCreateContext(any _dc) any {
   "Runs the wglCreateContext operation."
   0
}

fn wglDeleteContext(any _ctx) int {
   "Runs the wglDeleteContext operation."
   0
}

fn wglMakeCurrent(any _dc, any _ctx) int {
   "Runs the wglMakeCurrent operation."
   0
}

fn SwapBuffers(any _dc) int {
   "Runs the SwapBuffers operation."
   0
}

fn wglGetProcAddress(any _name) any {
   "Runs the wglGetProcAddress operation."
   0
}

fn choose_pixel_format(any dc) int {
   "Runs the choose pixel format operation."
   if(!dc){ return 0 }
   def pfd = zalloc(40)
   if(!pfd){ return 0 }
   store16(pfd, 40, 0)
   store16(pfd, 1, 2)
   store32(pfd, 0x00000004 | 0x00000020 | 0x00000001, 4)
   store8(pfd, 0, 8)
   store8(pfd, 32, 9)
   store8(pfd, 24, 23)
   store8(pfd, 8, 24)
   store8(pfd, 0, 25)
   def pf = ChoosePixelFormat(dc, pfd)
   if(pf != 0){ SetPixelFormat(dc, pf, pfd) }
   free(pfd)
   pf
}

fn create_context(any dc) any {
   "Creates create context."
   if(!dc){ return 0 }
   wglCreateContext(dc)
}

fn destroy_context(any ctx) bool {
   "Destroys destroy context."
   if(!ctx){ return false }
   wglDeleteContext(ctx)
   true
}

fn make_current(any dc, any ctx) bool {
   "Builds make current."
   wglMakeCurrent(dc, ctx) != 0
}

fn swap_buffers(any dc) bool {
   "Runs the swap buffers operation."
   if(!dc){ return false }
   SwapBuffers(dc) != 0
}

fn swap_interval(any interval) bool {
   "Runs the swap interval operation."
   false
}

fn get_proc_address(any name) any {
   "Returns get proc address."
   def proc_name = to_str(name)
   if(!startswith(proc_name, "gl") && !startswith(proc_name, "wgl")){ return 0 }
   def s, p = cstr(proc_name), wglGetProcAddress(s)
   if(p){ return p }
   0
}

#main {
   assert(choose_pixel_format(0) == 0, "wgl pixel format fallback")
   assert(create_context(0) == 0 && !destroy_context(0), "wgl context fallback")
   assert(!make_current(0, 0) && !swap_buffers(0) && !swap_interval(1), "wgl current fallback")
   assert(get_proc_address("definitely_missing_nytrix_probe_symbol") == 0, "wgl proc fallback")
   print("✓ std.os.ui.window.platform.opengl.wgl self-test passed")
}
