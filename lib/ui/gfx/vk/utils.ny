;; Auto-generated split Vulkan renderer component
module std.ui.gfx.vk.utils (
  _is_debug,
  _touch,
  _dbg_handle,
  _strcpy,
  set_mvp,
  _update_default_mvp
)
use std.core *
use std.core.mem *
use std.os *
use std.text.io as tio
use std.math *
use std.math.matrix *
use std.ui.glfw as ui_glfw
use std.ui.gfx.vulkan *
use std.ui.gfx.vk.state *

fn _is_debug(){
   "Returns true if high-level graphics debugging is enabled."
   def d = env("NY_GFX_DEBUG")
   if(d == "1" || d == "true"){ return true }
   false
}

fn _touch(...args){
   "Internal helper to mark arguments as used."
   0
}

fn _dbg_handle(label, h){
   "Prints a debug message for a Vulkan handle if debugging is enabled."
   if(!_is_debug()){ return 0 }
   print(f"Vulkan: {label} h={h}")
   0
}

fn _strcpy(dst, src){
   "Simple C-style string copy for raw buffers."
   mut i = 0
   while(true){
      def c = load8(src, i)
      store8(dst, c, i)
      if(c == 0){ break }
      i += 1
   }
}fn set_mvp(mat){
   "Updates the global Model-View-Projection matrix for the renderer."
   def data = vk_get(VK_CTX_CURRENT_MVP)
   if(!data){ return 0 }
   if(is_list(mat)){
      mat4_to_buffer(mat, data)
   } else {
      memcpy(data, mat, 64)
   }
   0
}

fn _update_default_mvp(win){
   "Recalculates the default orthographic projection matrix for the window."
   def w = float(get(win, 5, 800))
   def h = float(get(win, 6, 600))
   def proj = mat4_ortho(0.0, float(w), 0.0, float(h), -1.0, 1.0)
   set_mvp(proj)
}
