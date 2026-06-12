;; Keywords: render shader os ui
;; Backend-neutral shader-source handler for std.os.ui.render usage.
;; References:
;; - std.os.ui.render
;; - std.os.ui.render.matrix
module std.os.ui.render.shader(
   SHADER_BACKEND_VK450, SHADER_BACKEND_GL120, SHADER_BACKEND_GL330, SHADER_BACKEND_GLES300,
   normalize_backend_name, select_shader_defs, select_shader_source,
   parse_combined_shader, transpile_shader_defs, transpile_shader_source,
   shader_hash32, shader_sources, shader_watch
)

use std.core
use std.core.dict_mod
use std.core.str
use std.os (file_read, ticks)
use std.os.path as ospath

def SHADER_BACKEND_VK450 = "vkglsl450"
def SHADER_BACKEND_GL120 = "glsl120"
def SHADER_BACKEND_GL330 = "glsl330"
def SHADER_BACKEND_GLES300 = "glsl300es"

fn normalize_backend_name(any name) str {
   "Normalizes backend aliases to render shader targets."
   def n = lower(strip(to_str(name)))
   if(n == "vk" || n == "vulkan" || n == "glsl450" ||
      n == "vk450" || n == SHADER_BACKEND_VK450){
      return SHADER_BACKEND_VK450
   }
   if(n == "gl" || n == "opengl" || n == "gl120" || n == SHADER_BACKEND_GL120){
      return SHADER_BACKEND_GL120
   }
   if(n == "gl330" || n == "opengl330" || n == "glsl330" || n == SHADER_BACKEND_GL330){
      return SHADER_BACKEND_GL330
   }
   if(n == "webgl" || n == "webgl2" || n == "gles" || n == "gles300" ||
      n == "glsl300es" || n == SHADER_BACKEND_GLES300){
      return SHADER_BACKEND_GLES300
   }
   SHADER_BACKEND_VK450
}

fn _strip_known_versions(str src) str {
   mut out = str_replace(src, "\r\n", "\n")
   out = str_replace(out, "#version 120\n", "")
   out = str_replace(out, "#version 330\n", "")
   out = str_replace(out, "#version 330 core\n", "")
   out = str_replace(out, "#version 450\n", "")
   out = str_replace(out, "#version 450 core\n", "")
   out = str_replace(out, "#version 460\n", "")
   out = str_replace(out, "#version 460 core\n", "")
   out = str_replace(out, "#version 300 es\n", "")
   out
}

fn _vertex_to_vk450(str src) str { "#version 450\n" + _strip_known_versions(src) }

fn _fragment_to_vk450(str src) str {
   mut out = _strip_known_versions(src)
   if(str_contains(out, "gl_FragColor")){
      out = str_replace(out, "gl_FragColor", "fragColor")
      out = "layout(location = 0) out vec4 fragColor;\n" + out
   }
   if(!str_contains(out, "layout(location = 0) out")){
      if(str_contains(out, "out vec4")){ out = str_replace(out, "out vec4", "layout(location = 0) out vec4") }
   }
   "#version 450\n" + out
}

fn _vertex_to_gl120(str src) str { "#version 120\n" + _strip_known_versions(src) }
fn _fragment_to_gl120(str src) str { "#version 120\n" + _strip_known_versions(src) }

fn _vertex_to_gl330(str src) str { "#version 330 core\n" + _strip_known_versions(src) }

fn _fragment_to_gl330(str src) str {
   mut out = _strip_known_versions(src)
   if(str_contains(out, "gl_FragColor")){
      out = str_replace(out, "gl_FragColor", "fragColor")
      out = "out vec4 fragColor;\n" + out
   }
   "#version 330 core\n" + out
}

fn _vertex_to_gles300(str src) str { "#version 300 es\n" + _strip_known_versions(src) }

fn _fragment_to_gles300(str src) str {
   mut out = _strip_known_versions(src)
   if(!str_contains(out, "precision ")){ out = "precision mediump float;\n" + out }
   if(str_contains(out, "gl_FragColor")){
      out = str_replace(out, "gl_FragColor", "fragColor")
      out = "out vec4 fragColor;\n" + out
   }
   "#version 300 es\n" + out
}

fn _parse_marker(str line, str prefix) any {
   def trimmed = strip(line)
   if(!startswith(trimmed, prefix)){ return 0 }
   mut tail = strip(str_replace(trimmed, prefix, ""))
   if(tail.len == 0){ tail = "vkglsl450" }
   tail
}

fn parse_combined_shader(str combined_src) dict {
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

fn transpile_shader_defs(any defs) dict {
   "Ensures defs include Vulkan, OpenGL, and WebGL shader variants."
   if(!is_dict(defs)){ return dict(4) }
   mut out = dict_clone(defs)
   mut vs450 = out.get("vs_vkglsl450", 0)
   mut fs450 = out.get("fs_vkglsl450", 0)
   mut vs120 = out.get("vs_glsl120", 0)
   mut fs120 = out.get("fs_glsl120", 0)
   mut vs330 = out.get("vs_glsl330", 0)
   mut fs330 = out.get("fs_glsl330", 0)
   mut vs300es = out.get("vs_glsl300es", 0)
   mut fs300es = out.get("fs_glsl300es", 0)
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
   if(vs120){ out = out.set("vs_glsl120", _vertex_to_gl120(vs120)) } else {
      def vs_any120 = out.get("vs_glsl330", out.get("vs_vkglsl450", 0))
      if(vs_any120){ out = out.set("vs_glsl120", _vertex_to_gl120(vs_any120)) }
   }
   if(fs120){ out = out.set("fs_glsl120", _fragment_to_gl120(fs120)) } else {
      def fs_any120 = out.get("fs_glsl330", out.get("fs_vkglsl450", 0))
      if(fs_any120){ out = out.set("fs_glsl120", _fragment_to_gl120(fs_any120)) }
   }
   if(vs330){ out = out.set("vs_glsl330", _vertex_to_gl330(vs330)) } else {
      def vs_any330 = out.get("vs_glsl120", out.get("vs_vkglsl450", 0))
      if(vs_any330){ out = out.set("vs_glsl330", _vertex_to_gl330(vs_any330)) }
   }
   if(fs330){ out = out.set("fs_glsl330", _fragment_to_gl330(fs330)) } else {
      def fs_any330 = out.get("fs_glsl120", out.get("fs_vkglsl450", 0))
      if(fs_any330){ out = out.set("fs_glsl330", _fragment_to_gl330(fs_any330)) }
   }
   if(vs300es){ out = out.set("vs_glsl300es", _vertex_to_gles300(vs300es)) } else {
      def vs_any300es = out.get("vs_glsl330", out.get("vs_glsl120", out.get("vs_vkglsl450", 0)))
      if(vs_any300es){ out = out.set("vs_glsl300es", _vertex_to_gles300(vs_any300es)) }
   }
   if(fs300es){ out = out.set("fs_glsl300es", _fragment_to_gles300(fs300es)) } else {
      def fs_any300es = out.get("fs_glsl330", out.get("fs_glsl120", out.get("fs_vkglsl450", 0)))
      if(fs_any300es){ out = out.set("fs_glsl300es", _fragment_to_gles300(fs_any300es)) }
   }
   out
}

fn transpile_shader_source(str combined_src) dict {
   "Convenience wrapper to parse and transpile a combined shader string."
   transpile_shader_defs(parse_combined_shader(combined_src))
}

fn _read_text(any path) str {
   match file_read(path){
      ok(s) -> { return s }
      err(_) -> { return "" }
   }
}

fn _resolve_repo_asset(any rel) str {
   def raw = strip(to_str(rel))
   raw.len == 0 ? "" : ospath.resolve_repo_asset(raw)
}

fn shader_hash32(any s) int {
   "Returns a small stable hash for shader source change detection."
   if(!is_str(s)){ return 0 }
   mut h = 2166136261
   mut i = 0
   def n = s.len
   while(i < n){
      h = band(bxor(h, load8(s, i)) * 16777619, 2147483647)
      i += 1
   }
   h
}

fn shader_sources(any vert_rel, any frag_rel) list {
   "Loads vertex and fragment shader source from repo-relative or absolute paths."
   [
      _read_text(_resolve_repo_asset(vert_rel)),
      _read_text(_resolve_repo_asset(frag_rel)),
   ]
}

fn shader_watch(any state, any vert_rel, any frag_rel, any force=false, any poll_ns=250000000) dict {
   "Polls shader source files and returns a state dict with ready/changed/source fields."
   mut out = is_dict(state) ? state : dict(12)
   def now = ticks()
   def last = int(out.get("last_check", 0))
   if(!bool(force) && int(poll_ns) > 0 && last > 0 && (now - last) < int(poll_ns)){
      out["changed"] = false
      return out
   }
   def vert_path = _resolve_repo_asset(vert_rel)
   def frag_path = _resolve_repo_asset(frag_rel)
   def vert = _read_text(vert_path)
   def frag = _read_text(frag_path)
   def ready = vert.len > 0 && frag.len > 0
   def sig = ready ? (to_str(shader_hash32(vert)) + ":" + to_str(shader_hash32(frag)) + ":" + to_str(vert.len) + ":" + to_str(frag.len)) : ""
   def changed = ready && sig != to_str(out.get("sig", ""))
   out["vert_path"] = vert_path
   out["frag_path"] = frag_path
   out["vert"] = vert
   out["frag"] = frag
   out["sig"] = sig
   out["ready"] = ready
   out["changed"] = changed
   out["last_check"] = now
   out["error"] = ready ? "" : "missing shader source"
   out
}

fn select_shader_defs(any defs, str _backend="vkglsl450") dict {
   "Selects the appropriate shader variants for the specified backend."
   def backend = normalize_backend_name(_backend)
   defs = transpile_shader_defs(defs)
   def suffix = backend
   return {
      "backend": backend,
      "vs": defs.get("vs_" + suffix, ""),
      "fs": defs.get("fs_" + suffix, ""),
      "defs": defs
   }
}

fn select_shader_source(str combined_src, str backend="vkglsl450") dict {
   "Convenience wrapper to parse, transpile, and select shader sources."
   select_shader_defs(parse_combined_shader(combined_src), backend)
}

#main {
   def sel = select_shader_source("#vertex\nvoid main(){}\n#fragment\nvoid main(){}\n")
   assert(sel.get("backend", "") == SHADER_BACKEND_VK450 && sel.get("vs", "").contains("#version 450") && sel.get("fs", "").contains("#version 450"), "shader source selection")
   def gl_sel = select_shader_source("#vertex glsl120\nvoid main(){}\n#fragment glsl120\nvoid main(){ gl_FragColor = vec4(1.0); }\n", "webgl2")
   assert(gl_sel.get("backend", "") == SHADER_BACKEND_GLES300 && gl_sel.get("vs", "").contains("#version 300 es") && gl_sel.get("fs", "").contains("out vec4 fragColor"), "webgl shader source selection")
   assert(shader_hash32("abc") == shader_hash32("abc") && shader_hash32("abc") != shader_hash32("abd"), "shader hash")
   mut watch = dict(12)
   watch = shader_watch(watch, "etc/assets/shaders/ui/lit.vert.glsl", "etc/assets/shaders/ui/lit.frag.glsl", true, 0)
   assert(watch.get("ready", false) && watch.get("changed", false), "shader watch initial")
   watch = shader_watch(watch, "etc/assets/shaders/ui/lit.vert.glsl", "etc/assets/shaders/ui/lit.frag.glsl", true, 0)
   assert(watch.get("ready", false) && !watch.get("changed", true), "shader watch stable")
   print("✓ std.os.ui.render.shader self-test passed")
}
