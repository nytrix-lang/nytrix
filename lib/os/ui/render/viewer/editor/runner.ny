;; Keywords: editor runner build command output os ui render viewer text
;; Build and run command helpers for editor-integrated tooling.
;; References:
;; - std.os.ui.render.viewer.editor.terminal
module std.os.ui.render.viewer.editor.runner(command_for, run_file, run_text, section_at, output_name, output_text)
use std.core
use std.core.common as common
use std.core.str as str
use std.os (file_exists, file_remove, file_write, pid, ticks)
use std.os.path as ospath
use std.os.subprocess (run_capture)

fn _ext_key(str path) str {
   def ext = str.lower(ospath.extname(path))
   if ext.len <= 1 { return "" }
   mut out = ""
   mut i = 1
   while i < ext.len {
      def c = load8(ext, i)
      if (c >= 97 && c <= 122) || (c >= 48 && c <= 57) { out += chr(c) }
      else { out += "_" }
      i += 1
   }
   out
}

fn _replace_file(list words, str path) list {
   mut out = []
   mut used = false
   mut i = 0
   while i < words.len {
      def w = to_str(words.get(i, ""))
      if str.str_contains(w, "{file}") {
         out = out.append(str.str_replace(w, "{file}", path))
         used = true
      } else {
         out = out.append(w)
      }
      i += 1
   }
   if !used { out = out.append(path) }
   out
}

fn _configured(str path) list {
   def key = _ext_key(path)
   if key.len > 0 {
      def per_ext = common.env_trim("NY_EDITOR_RUN_" + str.upper(key))
      if per_ext.len > 0 { return _replace_file(str.split_words(per_ext), path) }
   }
   def global = common.env_trim("NY_EDITOR_RUN_COMMAND")
   if global.len > 0 { return _replace_file(str.split_words(global), path) }
   []
}

fn _default_for(str path) list {
   def ext = str.lower(ospath.extname(path))
   if ext == ".ny" {
      if file_exists("./build/release/ny") { return ["./build/release/ny", path] }
      return ["ny", path]
   }
   if ext == ".py" { return ["python3", path] }
   if ext == ".js" { return ["node", path] }
   if ext == ".ts" { return ["deno", "run", path] }
   if ext == ".lua" { return ["lua", path] }
   if ext == ".rb" { return ["ruby", path] }
   if ext == ".sh" { return ["sh", path] }
   if ext == ".pl" { return ["perl", path] }
   if ext == ".php" { return ["php", path] }
   []
}

fn command_for(str path) list {
   "Returns argv for running `path`, using NY_EDITOR_RUN_<EXT> or NY_EDITOR_RUN_COMMAND first."
   if path.len <= 0 { return [] }
   def cfg = _configured(path)
   cfg.len > 0 ? cfg : _default_for(path)
}

fn output_name(str path) str {
   "*run: " + (path.len > 0 ? ospath.basename(path) : "untitled") + "*"
}

fn output_text(dict res) str {
   def argv = res.get("argv", [])
   def code = int(res.get("code", 0))
   def stdout = to_str(res.get("stdout", ""))
   "$ " + str.join(argv, " ") + "\nexit " + to_str(code) + "\n\n" + stdout
}

fn run_file(str path) dict {
   "Runs a file and returns {ok, code, stdout, argv, path, error}."
   def argv = command_for(path)
   if argv.len <= 0 {
      return {"ok": false, "code": 127, "stdout": "", "argv": [], "path": path, "error": "no runner"}
   }
   def res = run_capture(argv, [], nil, false)
   res["path"] = path
   res
}

fn _temp_run_path(str source_path, str label) str {
   def ext = ospath.extname(source_path)
   def suffix = ext.len > 0 ? ext : ".txt"
   ospath.join(ospath.temp_dir(), "ny_editor_run_" + to_str(pid()) + "_" + to_str(ticks()) + "_" + label + suffix)
}

fn run_text(str source_path, str text, str label="selection") dict {
   "Runs text through the configured runner for `source_path`, preserving the source extension through a temp file."
   if str.strip(text).len <= 0 {
      return {"ok": false, "code": 0, "stdout": "", "argv": [], "path": source_path, "error": "empty input", "label": label}
   }
   def tmp = _temp_run_path(source_path, label)
   def body = str.endswith(text, "\n") ? text : text + "\n"
   match file_write(tmp, body) {
      ok(_) -> {
         mut res = run_file(tmp)
         res["source_path"] = source_path
         res["label"] = label
         match file_remove(tmp) { ok(_) -> {} err(_) -> {} }
         return res
      }
      err(e) -> {
         {"ok": false, "code": 1, "stdout": "run temp write failed: " + to_str(e) + "\n", "argv": [], "path": source_path, "error": to_str(e), "label": label}
      }
   }
}

fn _section_marker(str line) bool {
   def s = str.strip(line)
   s == "%%" ||
   str.startswith(s, "# %%") ||
   str.startswith(s, "// %%") ||
   str.startswith(s, ";; %%") ||
   str.startswith(s, "-- %%")
}

fn _join_range(list lines, int start, int end) str {
   mut out = []
   mut i = start
   while i <= end && i < lines.len {
      out = out.append(to_str(lines.get(i, "")))
      i += 1
   }
   str.join(out, "\n")
}

fn section_at(list lines, int row) dict {
   "Returns the current marked section using # %%, // %%, ;; %%, -- %%, or %% markers."
   if lines.len <= 0 { return {"found": false, "text": "", "start": 0, "end": 0} }
   row = min(max(row, 0), lines.len - 1)
   mut any_marker = false
   mut prev = -1
   mut next = lines.len
   mut i = 0
   while i < lines.len {
      if _section_marker(to_str(lines.get(i, ""))) {
         any_marker = true
         if i <= row { prev = i }
         elif i > row { next = i break }
      }
      i += 1
   }
   if !any_marker { return {"found": false, "text": "", "start": 0, "end": 0} }
   def start = prev >= 0 ? prev + 1 : 0
   def end = max(start - 1, next - 1)
   {"found": true, "text": _join_range(lines, start, end), "start": start, "end": end}
}

#main {
   assert(command_for("a.ny").len >= 2, "runner ny default")
   assert(output_name("foo.py") == "*run: foo.py*", "runner output name")
   def sec = section_at(["one", "# %%", "two", "three", "# %%", "four"], 2)
   assert(sec.get("found", false) && to_str(sec.get("text", "")) == "two\nthree", "runner section")
   print("✓ viewer editor runner test passed")
}
