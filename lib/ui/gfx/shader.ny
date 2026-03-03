;; Keywords: ui gfx shader transpiler
;; Reduced shader-source handler for Vulkan std.ui.gfx usage.

module std.ui.gfx.shader (
   SHADER_BACKEND_VK450,
   normalize_backend_name, select_shader_defs, select_shader_source,
   parse_combined_shader, transpile_shader_defs, transpile_shader_source
)

use std.core *
use std.core.dict_mod *
use std.text *

def SHADER_BACKEND_VK450 = "vkglsl450"

fn normalize_backend_name(name){
   "Normalizes backend aliases to Vulkan."
   def n = lower(strip(to_str(name)))
   if(n == "vk" || n == "vulkan" || n == "glsl450" ||
      n == "vk450" || n == SHADER_BACKEND_VK450){
      return SHADER_BACKEND_VK450
   }
   SHADER_BACKEND_VK450
}

fn _slice_from(s, start){
   "Internal helper to slice a string from start to end."
   def n = str_len(s)
   if(start <= 0){ return s }
   if(start >= n){ return "" }
   mut out = malloc(n - start + 1)
   init_str(out, n - start)
   mut i = 0
   while(i < (n - start)){
      store8(out, load8(s, start + i), i)
      i += 1
   }
   store8(out, 0, n - start)
   out
}

fn _strip_known_versions(src){
   "Removes common #version directives from a GLSL string."
   mut out = replace_all(src, "\r\n", "\n")
   out = replace_all(out, "#version 120\n", "")
   out = replace_all(out, "#version 330\n", "")
   out = replace_all(out, "#version 330 core\n", "")
   out = replace_all(out, "#version 450\n", "")
   out = replace_all(out, "#version 450 core\n", "")
   out = replace_all(out, "#version 460\n", "")
   out = replace_all(out, "#version 460 core\n", "")
   out
}

fn _vertex_to_vk450(src){
   "#version 450\n" + _strip_known_versions(src)
}

fn _fragment_to_vk450(src){
   "Heuristically transpiles a legacy GLSL fragment shader to Vulkan-GLSL 450."
   mut out = _strip_known_versions(src)
   if(str_contains(out, "gl_FragColor")){
      out = replace_all(out, "gl_FragColor", "fragColor")
      out = "layout(location = 0) out vec4 fragColor;\n" + out
   }
   if(!str_contains(out, "layout(location = 0) out")){
       ; Ensure output is declared for VK if missing
       if(str_contains(out, "out vec4")){
           out = str_replace(out, "out vec4", "layout(location = 0) out vec4")
       }
   }
   "#version 450\n" + out
}

fn _parse_marker(line, prefix){
   "Parses a shader stage marker (e.g. #vertex [backend])."
   def trimmed = strip(line)
   if(!startswith(trimmed, prefix)){ return 0 }
   mut tail = strip(str_replace(trimmed, prefix, ""))
   if(str_len(tail) == 0){ tail = "vkglsl450" }
   tail
}

fn parse_combined_shader(combined_src){
   "Parses a single file containing multiple shader stages into a stage dictionary."
   mut defs = dict(8)
   def lines = split(replace_all(combined_src, "\r\n", "\n"), "\n")
   mut target = ""
   mut src = ""
   mut i = 0
   while(i < len(lines)){
      def line = get(lines, i)
      def vs_tag = _parse_marker(line, "#vertex")
      def fs_tag = _parse_marker(line, "#fragment")
      if(vs_tag){
         if(str_len(target) > 0){ defs = dict_set(defs, target, src) }
         target = "vs_" + vs_tag
         src = ""
      } elif(fs_tag){
         if(str_len(target) > 0){ defs = dict_set(defs, target, src) }
         target = "fs_" + fs_tag
         src = ""
      } else {
         if(str_len(target) > 0){ src = src + line + "\n" }
      }
      i += 1
   }
   if(str_len(target) > 0){ defs = dict_set(defs, target, src) }
   defs
}

fn transpile_shader_defs(defs){
   "Ensures defs include Vulkan-GLSL 450 variants."
   if(!is_dict(defs)){ return dict(4) }
   mut out = dict_clone(defs)
   mut vs450 = dict_get(out, "vs_vkglsl450", 0)
   mut fs450 = dict_get(out, "fs_vkglsl450", 0)
   if(!vs450){
      def vs_any = dict_get(out, "vs_glsl330", dict_get(out, "vs_glsl120", 0))
      if(vs_any){
         out = dict_set(out, "vs_vkglsl450", _vertex_to_vk450(vs_any))
      }
   }
   if(!fs450){
      def fs_any = dict_get(out, "fs_glsl330", dict_get(out, "fs_glsl120", 0))
      if(fs_any){
         out = dict_set(out, "fs_vkglsl450", _fragment_to_vk450(fs_any))
      }
   }
   out
}

fn transpile_shader_source(combined_src){
   "Convenience wrapper to parse and transpile a combined shader string."
   transpile_shader_defs(parse_combined_shader(combined_src))
}

fn select_shader_defs(defs, _backend=SHADER_BACKEND_VK450){
   "Selects the appropriate shader variants for the specified backend."
   defs = transpile_shader_defs(defs)
   mut out = dict(8)
   out = dict_set(out, "backend", SHADER_BACKEND_VK450)
   out = dict_set(out, "vs", dict_get(defs, "vs_vkglsl450", ""))
   out = dict_set(out, "fs", dict_get(defs, "fs_vkglsl450", ""))
   out = dict_set(out, "defs", defs)
   out
}

fn select_shader_source(combined_src, backend=SHADER_BACKEND_VK450){
   "Convenience wrapper to parse, transpile, and select shader sources."
   select_shader_defs(parse_combined_shader(combined_src), backend)
}

if(comptime{__main()}){
   def src = "#vertex\nvoid main(){}\n#fragment\nvoid main(){}\n"
   def sel = select_shader_source(src)
   assert(dict_get(sel, "backend", "") == SHADER_BACKEND_VK450, "shader select vk")
   assert(str_contains(dict_get(sel, "vs", ""), "#version 450"), "shader vs version")
   print("✓ std.ui.gfx.shader (VK only) tests passed")
}
