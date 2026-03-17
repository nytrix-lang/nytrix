;; Keywords: render shader
;; Reduced shader-source handler for Vulkan std.os.ui.render usage.
module std.os.ui.render.shader(SHADER_BACKEND_VK450, normalize_backend_name, select_shader_defs, select_shader_source, parse_combined_shader, transpile_shader_defs, transpile_shader_source)
use std.core
use std.core.dict_mod
use std.core.str

def SHADER_BACKEND_VK450 = "vkglsl450"

fn normalize_backend_name(any: name): str {
   "Normalizes backend aliases to Vulkan."
   def n = lower(strip(to_str(name)))
   if(n == "vk" || n == "vulkan" || n == "glsl450" ||
      n == "vk450" || n == SHADER_BACKEND_VK450){
      return SHADER_BACKEND_VK450
   }
   SHADER_BACKEND_VK450
}

fn _strip_known_versions(str: src): str {
   mut out = str_replace(src, "\r\n", "\n")
   out = str_replace(out, "#version 120\n", "")
   out = str_replace(out, "#version 330\n", "")
   out = str_replace(out, "#version 330 core\n", "")
   out = str_replace(out, "#version 450\n", "")
   out = str_replace(out, "#version 450 core\n", "")
   out = str_replace(out, "#version 460\n", "")
   out = str_replace(out, "#version 460 core\n", "")
   out
}

fn _vertex_to_vk450(str: src): str { "#version 450\n" + _strip_known_versions(src) }

fn _fragment_to_vk450(str: src): str {
   mut out = _strip_known_versions(src)
   if(str_contains(out, "gl_FragColor")){
      out = str_replace(out, "gl_FragColor", "fragColor")
      out = "layout(location = 0) out vec4 fragColor;\n" + out
   }
   if(!str_contains(out, "layout(location = 0) out")){
      ; Ensure output is declared for VK if missing
      if(str_contains(out, "out vec4")){ out = str_replace(out, "out vec4", "layout(location = 0) out vec4") }
   }
   "#version 450\n" + out
}

fn _parse_marker(str: line, str: prefix): any {
   def trimmed = strip(line)
   if(!startswith(trimmed, prefix)){ return 0 }
   mut tail = strip(str_replace(trimmed, prefix, ""))
   if(tail.len == 0){ tail = "vkglsl450" }
   tail
}

fn parse_combined_shader(str: combined_src): dict {
   "Parses a single file containing multiple shader stages into a stage dictionary."
   mut defs = dict(8)
   def lines = split(str_replace(combined_src, "\r\n", "\n"), "\n")
   mut target = ""
   mut src = ""
   mut i = 0
   def lines_n = lines.len
   while(i < lines_n){
      def line = lines.get(i)
      def vs_tag = _parse_marker(line, "#vertex")
      def fs_tag = _parse_marker(line, "#fragment")
      if(vs_tag){
         if(target.len > 0){ defs = defs.set(target, src) }
         target = "vs_" + vs_tag
         src = ""
      } elif(fs_tag){
         if(target.len > 0){ defs = defs.set(target, src) }
         target = "fs_" + fs_tag
         src = ""
      } else {
         if(target.len > 0){ src = src + line + "\n" }
      }
      i += 1
   }
   if(target.len > 0){ defs = defs.set(target, src) }
   defs
}

fn transpile_shader_defs(any: defs): dict {
   "Ensures defs include Vulkan-GLSL 450 variants."
   if(!is_dict(defs)){ return dict(4) }
   mut out = dict_clone(defs)
   mut vs450 = out.get("vs_vkglsl450", 0)
   mut fs450 = out.get("fs_vkglsl450", 0)
   if(vs450){
      out = out.set("vs_vkglsl450", _vertex_to_vk450(vs450))
   } elif(!vs450){
      def vs_any = out.get("vs_glsl330", out.get("vs_glsl120", 0))
      if(vs_any){ out = out.set("vs_vkglsl450", _vertex_to_vk450(vs_any)) }
   }
   if(fs450){
      out = out.set("fs_vkglsl450", _fragment_to_vk450(fs450))
   } elif(!fs450){
      def fs_any = out.get("fs_glsl330", out.get("fs_glsl120", 0))
      if(fs_any){ out = out.set("fs_vkglsl450", _fragment_to_vk450(fs_any)) }
   }
   out
}

fn transpile_shader_source(str: combined_src): dict {
   "Convenience wrapper to parse and transpile a combined shader string."
   transpile_shader_defs(parse_combined_shader(combined_src))
}

fn select_shader_defs(any: defs, str: _backend="vkglsl450"): dict {
   "Selects the appropriate shader variants for the specified backend."
   defs = transpile_shader_defs(defs)
   return {
      "backend": SHADER_BACKEND_VK450,
      "vs": defs.get("vs_vkglsl450", ""),
      "fs": defs.get("fs_vkglsl450", ""),
      "defs": defs
   }
}

fn select_shader_source(str: combined_src, str: backend="vkglsl450"): dict {
   "Convenience wrapper to parse, transpile, and select shader sources."
   select_shader_defs(parse_combined_shader(combined_src), backend)
}
