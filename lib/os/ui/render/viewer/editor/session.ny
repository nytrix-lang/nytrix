;; Keywords: editor session files buffers tabs os ui render viewer text
;; Editor session state for open files, buffers, and workspace navigation.
;; References:
;; - std.os.ui.render.viewer.editor.core
module std.os.ui.render.viewer.editor.session(
   file_kind, read_text, read_buffer, write_text,
   seed_buffers, mark_clean, set_status,
   append_file, open_file, open_or_toggle_project_row, project_entry_for_path,
   set_output_buffer, save, run_current_file, run_text, check_current_file, format_current_file, debug_current_file
)

use std.core
use std.core.common as common
use std.core.str as str
use std.math (max, min)
use std.os (file_exists, file_read, file_remove, file_write, pid, ticks)
use std.os.fs (is_dir)
use std.os.path as ospath
use std.os.sys as sys
use std.os.ui.render.viewer.editor as ed
use std.os.ui.render.viewer.editor.project as project
use std.os.ui.render.viewer.editor.runner as runner
use std.os.ui.render.viewer.editor.tools as tools

fn set_status(dict st, str msg) dict {
   st["status"] = msg
   st["status_timer"] = 1.5
   st
}

fn file_kind(str path) str {
   project.file_kind(ospath.basename(path))
}

fn _kind_is_preview(str kind) bool {
   kind == "image" || kind == "model" || kind == "font" ||
   kind == "audio" || kind == "video" || kind == "archive" || kind == "binary"
}

fn read_text(str path) str {
   if(path.len <= 0 || !file_exists(path)){ return "" }
   match file_read(path){ ok(v) -> to_str(v) err(_) -> "" }
}

fn _max_edit_bytes() int {
   common.env_int_clamped("NY_EDITOR_MAX_EDIT_BYTES", 2 * 1024 * 1024, 4096, 128 * 1024 * 1024)
}

fn _max_edit_lines() int {
   common.env_int_clamped("NY_EDITOR_MAX_EDIT_LINES", 50000, 1000, 1000000)
}

fn _line_count_exceeds(str data, int max_lines) bool {
   mut lines = 1
   mut i = 0
   while(i < data.len){
      if(load8(data, i) == 10){
         lines += 1
         if(lines > max_lines){ return true }
      }
      i += 1
   }
   false
}

fn _line_prefix(str data, int max_lines) str {
   mut lines = 1
   mut i = 0
   while(i < data.len){
      if(load8(data, i) == 10){
         lines += 1
         if(lines > max_lines){ return str.str_slice(data, 0, i) }
      }
      i += 1
   }
   data
}

fn _read_head(str path, int max_bytes) dict {
   if(path.len <= 0 || max_bytes <= 0 || !file_exists(path)){ return {"ok": false, "text": "", "truncated": false} }
   def want = max_bytes + 1
   match sys.sys_open(ospath.normalize(path), 0, 0){
      ok(fd) -> {
         def buf = malloc(want + 32)
         if(buf == 0){
            sys.sys_close_quiet(fd)
            return {"ok": false, "text": "", "truncated": false}
         }
         mut total = 0
         mut failed = false
         while(total < want){
            match sys.sys_read(fd, ptr_add(buf, total), want - total){
               ok(n) -> {
                  if(n <= 0){ break }
                  total += n
               }
               err(_) -> {
                  failed = true
                  break
               }
            }
         }
         sys.sys_close_quiet(fd)
         if(failed){
            free(buf)
            return {"ok": false, "text": "", "truncated": false}
         }
         if(total <= 0){
            free(buf)
            return {"ok": true, "text": "", "truncated": false}
         }
         def used = min(total, max_bytes)
         {"ok": true, "text": init_str(buf, used), "truncated": total > max_bytes, "bytes": used}
      }
      err(_) -> { {"ok": false, "text": "", "truncated": false} }
   }
}

fn _binary_like(str data) bool {
   if(data.len == 0){ return false }
   if(!str.utf8_valid(data)){ return true }
   def n = min(data.len, 4096)
   mut bad = 0
   mut i = 0
   while(i < n){
      def c = load8(data, i) & 255
      if(c == 0){ return true }
      if(c < 32 && c != 9 && c != 10 && c != 13){ bad += 1 }
      i += 1
   }
   bad > max(8, n / 80)
}

fn _hex_preview(str data, int limit=96) str {
   def n = min(data.len, limit)
   mut out = ""
   mut i = 0
   while(i < n){
      if(i > 0){ out += (i % 16 == 0 ? "\n" : " ") }
      out += str.to_hex(load8(data, i) & 255, 2)
      i += 1
   }
   if(data.len > n){ out += "\n..." }
   out
}

fn _preview_text(str path, str kind, str data="", str reason="") str {
   def name = ospath.basename(path)
   def ext = ospath.extname(name)
   mut out = "Preview: " + name + "\n"
   out += "path: " + path + "\n"
   out += "kind: " + (kind.len > 0 ? kind : "file") + (ext.len > 0 ? " (" + ext + ")" : "") + "\n"
   if(reason.len > 0){ out += "note: " + reason + "\n" }
   if(data.len > 0){
      out += "bytes loaded: " + to_str(data.len) + "\n\n"
      out += "hex:\n" + _hex_preview(data)
   } else {
      out += "\nThis file type is shown as a read-only preview to keep the editor buffer safe."
   }
   out
}

fn _preview_buffer(str path, str kind, str data="", str reason="") dict {
   mut b = ed.buffer(ospath.basename(path), "", _preview_text(path, kind, data, reason))
   b["source_path"] = path
   b["readonly"] = true
   b["kind"] = kind
   b
}

fn _large_text_buffer(str path, str data, str note) dict {
   mut text = "Preview: " + ospath.basename(path) + "\n"
   text += "path: " + path + "\n"
   text += "kind: large text\n"
   text += "note: " + note + "\n\n"
   text += data
   mut b = ed.buffer(ospath.basename(path), "", text)
   b["source_path"] = path
   b["readonly"] = true
   b["kind"] = "large-text"
   b
}

fn read_buffer(str path) dict {
   if(path.len <= 0 || !file_exists(path)){ return ed.buffer("missing", "", "") }
   if(is_dir(path)){ return _preview_buffer(path, "dir", "", "directory") }
   def kind = file_kind(path)
   if(_kind_is_preview(kind)){ return _preview_buffer(path, kind, "", "non-text file") }
   def max_bytes = _max_edit_bytes()
   def rd = _read_head(path, max_bytes)
   if(!rd.get("ok", false)){ return _preview_buffer(path, "file", "", "read failed") }
   def data = to_str(rd.get("text", ""))
   if(_binary_like(data)){ return _preview_buffer(path, "binary", data, "binary bytes detected") }
   if(rd.get("truncated", false)){ return _large_text_buffer(path, data, "showing first " + to_str(max_bytes) + " bytes; set NY_EDITOR_MAX_EDIT_BYTES to edit more") }
   def max_lines = _max_edit_lines()
   if(_line_count_exceeds(data, max_lines)){
      return _large_text_buffer(path, _line_prefix(data, max_lines), "showing first " + to_str(max_lines) + " lines; set NY_EDITOR_MAX_EDIT_LINES to edit more")
   }
   mut b = ed.buffer(ospath.basename(path), path, data)
   b["kind"] = kind
   b
}

fn write_text(str path, str text) bool {
   if(path.len <= 0){ return false }
   match file_write(path, text){ ok(_) -> true err(_) -> false }
}

fn seed_buffers(list args, str fallback="README.md") list {
   mut out = []
   if(args.len > 1){
      def path = to_str(args.get(1, ""))
      if(file_exists(path)){ out = out.append(read_buffer(path)) }
   }
   if(out.len == 0 && fallback.len > 0 && file_exists(fallback)){
      out = out.append(read_buffer(fallback))
   }
   if(out.len == 0){ out = out.append(ed.buffer("untitled.ny", "", "")) }
   out
}

fn mark_clean(dict st) dict {
   mut bs = st.get("buffers", [])
   mut b = ed.current_buffer(st)
   b["dirty"] = false
   bs[int(st.get("active", 0))] = b
   st["buffers"] = bs
   st
}

fn _buffer_index_for_path(dict st, str path) int {
   def want = ospath.normalize(path)
   def bs = st.get("buffers", [])
   mut i = 0
   while(i < bs.len){
      def b = bs.get(i, {})
      if(ospath.normalize(to_str(b.get("path", ""))) == want){ return i }
      if(ospath.normalize(to_str(b.get("source_path", ""))) == want){ return i }
      i += 1
   }
   -1
}

fn append_file(dict st, str path) dict {
   if(path.len <= 0 || !file_exists(path)){ return set_status(st, "open failed") }
   mut bs = st.get("buffers", [])
   def b = read_buffer(path)
   bs = bs.append(b)
   st["buffers"] = bs
   st = ed.select_buffer(st, bs.len - 1)
   set_status(st, (b.get("readonly", false) ? "preview " : "opened ") + ospath.basename(path))
}

fn open_file(dict st, str path) dict {
   if(path.len <= 0){ return st }
   def idx = _buffer_index_for_path(st, path)
   if(idx >= 0){
      st = ed.select_buffer(st, idx)
      return set_status(st, "buffer " + ospath.basename(path))
   }
   append_file(st, path)
}

fn project_entry_for_path(dict model, str path) dict {
   def want = ospath.normalize(path)
   def rows = project.tree(model)
   mut i = 0
   while(i < rows.len){
      def e = rows.get(i, {})
      if(ospath.normalize(to_str(e.get("path", ""))) == want){ return e }
      i += 1
   }
   dict(0)
}

fn open_or_toggle_project_row(dict st, dict model, dict entry) dict {
   if(entry.get("dir", false)){
      model = project.toggle(model, to_str(entry.get("rel", "")))
   } else {
      st = open_file(st, to_str(entry.get("path", "")))
   }
   {"st": st, "project": model}
}

fn set_output_buffer(dict st, str name, str text) dict {
   mut bs = st.get("buffers", [])
   mut hit = -1
   mut i = 0
   while(i < bs.len){
      def row = bs.get(i, dict(0))
      if(is_dict(row) && to_str(row.get("name", "")) == name){ hit = i break }
      i += 1
   }
   if(hit >= 0){
      mut b = bs.get(hit)
      b["lines"] = ed.split_lines(text)
      b["dirty"] = false
      bs[hit] = b
      st["buffers"] = bs
      st = ed.select_buffer(st, hit)
   } else {
      bs = bs.append(ed.buffer(name, "", text))
      st["buffers"] = bs
      st = ed.select_buffer(st, bs.len - 1)
   }
   st
}

fn save(dict st) dict {
   def b = ed.current_buffer(st)
   if(b.get("readonly", false)){ return set_status(st, "read-only preview") }
   def path = to_str(b.get("path", ""))
   if(path.len <= 0){ return set_status(st, "memory buffer") }
   if(write_text(path, ed.join_lines(ed.current_lines(st)))){
      return set_status(mark_clean(st), "saved " + ospath.basename(path))
   }
   set_status(st, "save failed")
}

fn _tool_output(dict st, str label, dict res) dict {
   def argv = res.get("argv", [])
   def head = "$ " + (is_list(argv) ? str.join(argv, " ") : "") + "\n\n"
   st = set_output_buffer(st, "*" + label + "*", head + to_str(res.get("stdout", "")))
   set_status(st, res.get("ok", false) ? label + " ok" : label + " exit " + to_str(int(res.get("code", 0))))
}

fn run_current_file(dict st) dict {
   st = save(st)
   def path = to_str(ed.current_buffer(st).get("path", ""))
   if(path.len <= 0){ return set_status(st, "save before run") }
   st = set_status(st, "running " + ospath.basename(path))
   def res = runner.run_file(path)
   st = set_output_buffer(st, runner.output_name(path), runner.output_text(res))
   set_status(st, res.get("ok", false) ? "run ok" : "run exit " + to_str(int(res.get("code", 0))))
}

fn run_text(dict st, str path, str text, str label="selection") dict {
   if(path.len <= 0){ return set_status(st, "save before run") }
   if(str.strip(text).len <= 0){ return set_status(st, "nothing to run") }
   st = set_status(st, "running " + label + " " + ospath.basename(path))
   def res = runner.run_text(path, text, label)
   st = set_output_buffer(st, runner.output_name(path), runner.output_text(res))
   set_status(st, res.get("ok", false) ? "run " + label + " ok" : "run " + label + " exit " + to_str(int(res.get("code", 0))))
}

fn check_current_file(dict st) dict {
   st = save(st)
   def path = to_str(ed.current_buffer(st).get("path", ""))
   if(path.len <= 0){ return set_status(st, "save before check") }
   _tool_output(st, "diagnostics: " + ospath.basename(path), tools.check_file(path))
}

fn format_current_file(dict st) dict {
   st = save(st)
   def b = ed.current_buffer(st)
   def path = to_str(b.get("path", ""))
   if(path.len <= 0){ return set_status(st, "save before format") }
   def res = tools.format_file(path)
   if(res.get("ok", false)){
      mut bs = st.get("buffers", [])
      mut cur = ed.current_buffer(st)
      cur["lines"] = ed.split_lines(read_text(path))
      cur["dirty"] = false
      bs[int(st.get("active", 0))] = cur
      st["buffers"] = bs
   }
   _tool_output(st, "format: " + ospath.basename(path), res)
}

fn debug_current_file(dict st) dict {
   st = save(st)
   def path = to_str(ed.current_buffer(st).get("path", ""))
   if(path.len <= 0){ return set_status(st, "save before debug") }
   _tool_output(st, "debug: " + ospath.basename(path), tools.debug_file(path))
}

#main {
   mut st = ed.state(seed_buffers([]))
   st = set_output_buffer(st, "*test*", "ok")
   assert(ed.current_buffer(st).get("name", "") == "*test*", "output buffer")
   def p = ospath.join(ospath.temp_dir(), "ny_editor_session_" + to_str(pid()) + "_" + to_str(ticks()) + ".txt")
   unwrap(file_write(p, str.repeat("x", 512)))
   def rd = _read_head(p, 64)
   assert(rd.get("ok", false) && rd.get("truncated", false) && to_str(rd.get("text", "")).len == 64, "bounded file read")
   unwrap(file_remove(p))
}
