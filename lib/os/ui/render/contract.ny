;; Keywords: render contract backend portable os ui
;; Backend-neutral rendering contract metadata. Keep this module lightweight so
;; compile-time contract probes do not need to load Vulkan/OpenGL/window stacks.
module core.os.ui.render.contract(portable_contract, backend_name)
use core

fn backend_name(int backend) str {
   "Returns the stable public name for a renderer backend id."
   if backend == 1 { return "opengl" }
   if backend == 2 { return "vulkan" }
   if backend == 3 { return "mock" }
   "none"
}

fn portable_contract() dict {
   "Returns the stable rendering subset shared by native OpenGL/Vulkan and browser WebGL2 paths."
   {
      "version": 1,
      "coordinate_system": "2d-top-left-pixels-3d-right-handed",
      "frame": {"begin": true, "clear": true, "draw": true, "present": true},
      "primitives": {"rect": true, "line": true, "triangle": true, "texture": true, "text": true},
      "depth_3d": true,
      "alpha_blend": "source-alpha",
      "backend_specific": ["vulkan", "opengl", "webgl2"],
      "not_guaranteed": ["raw-vulkan", "compute", "advanced-materials", "desktop-font-parity"]
   }
}
