;; Keywords: editor tools toolbar commands formatting os ui render viewer text
;; Toolbar and tool helpers for editor commands, formatting, and navigation.
;; References:
;; - std.os.ui.render.viewer.editor.commands
module std.os.ui.render.viewer.editor.tools(
   ny_command, lsp_command, check_file, format_file, debug_command_for, debug_file, status
)

use std.core
use std.core.str as str
use std.os (env, file_exists)
use std.os.path as ospath
use std.os.subprocess (run_capture)

fn _tool(str local, str fallback) str {
   file_exists(local) ? local : fallback
}

fn _env_key(str prefix, str path) str {
   def ext = str.upper(str.str_replace(ospath.extname(path), ".", ""))
   prefix + (ext.len > 0 ? ext : "FILE")
}

fn _configured_argv(str key, str path) list {
   def raw = env(key)
   if !is_str(raw) || raw.len <= 0 { return [] }
   mut out = []
   def parts = str.split_words(to_str(raw))
   mut i = 0
   while i < parts.len {
      out = out.append(str.str_replace(to_str(parts.get(i, "")), "{file}", path))
      i += 1
   }
   out
}

;; Returns the result of the `ny_command` operation.
fn ny_command() str {
   _tool("./build/release/ny", "ny")
}

;; Returns the result of the `lsp_command` operation.
fn lsp_command() str {
   _tool("./build/release/ny-lsp", "ny-lsp")
}

fn _fmt_command() str {
   _tool("./build/release/ny-fmt", "ny-fmt")
}

fn check_file(str path) dict {
   "Runs Nytrix diagnostics for a source file."
   if path.len <= 0 || !file_exists(path) { return {"ok": false, "code": 1, "stdout": "missing file: " + path, "argv": []} }
   def argv = [ny_command(), "--diag-compact", "--collect-errors", "-emit-only", path]
   def first = run_capture(argv, [], nil, false)
   if str.lower(ospath.extname(path)) != ".ny" { return first }
   def strict_argv = [ny_command(), "--diag-compact", "--collect-errors", "--ownership-strict", "-emit-only", path]
   def strict = run_capture(strict_argv, [], nil, false)
   def ok = bool(first.get("ok", false)) && bool(strict.get("ok", false))
   {
      "ok": ok,
      "code": ok ? 0 : (int(first.get("code", 0)) != 0 ? int(first.get("code", 0)) : int(strict.get("code", 1))),
      "stdout": "$ " + str.join(argv, " ") + "\n" + to_str(first.get("stdout", "")) +
      "\n$ " + str.join(strict_argv, " ") + "\n" + to_str(strict.get("stdout", "")),
      "argv": strict_argv
   }
}

fn format_file(str path) dict {
   "Formats a source file using ny-fmt when available."
   if path.len <= 0 || !file_exists(path) { return {"ok": false, "code": 1, "stdout": "missing file: " + path, "argv": []} }
   def argv = [_fmt_command(), path]
   run_capture(argv, [], nil, false)
}

fn debug_command_for(str path) list {
   "Returns a configured debug command for a file. Override with NY_EDITOR_DAP_<EXT> or NY_EDITOR_DAP_COMMAND."
   if path.len <= 0 { return [] }
   def specific = _configured_argv(_env_key("NY_EDITOR_DAP_", path), path)
   if specific.len > 0 { return specific }
   def generic = _configured_argv("NY_EDITOR_DAP_COMMAND", path)
   if generic.len > 0 { return generic }
   ["gdb", "--args", ny_command(), "-g", path]
}

fn debug_file(str path) dict {
   "Runs a configured non-interactive debug command if provided; otherwise returns the command hint."
   def specific = _configured_argv(_env_key("NY_EDITOR_DAP_", path), path)
   def generic = _configured_argv("NY_EDITOR_DAP_COMMAND", path)
   def argv = specific.len > 0 ? specific : generic
   if argv.len <= 0 {
      return {"ok": true, "code": 0, "stdout": "debug command: " + str.join(debug_command_for(path), " "), "argv": debug_command_for(path)}
   }
   run_capture(argv, [], nil, false)
}

;; Returns the result of the `status` operation.
fn status() dict {
   {
      "ny": ny_command(),
      "lsp": lsp_command(),
      "ny_ok": file_exists(ny_command()) || ny_command() == "ny",
      "lsp_ok": file_exists(lsp_command()) || lsp_command() == "ny-lsp",
      "fmt": _fmt_command(),
   }
}

#main {
   def s = status()
   assert(to_str(s.get("ny", "")).len > 0 && to_str(s.get("lsp", "")).len > 0, "editor tools status")
   assert(debug_command_for("main.ny").len > 0, "editor debug command")
   print("✓ viewer editor tools test passed")
}
