;; Keywords: editor lsp diagnostics symbols language os ui render viewer text
;; Language-service style diagnostics, symbols, and navigation helpers for editor buffers.
;; References:
;; - std.os.ui.render.viewer.editor.outline
module std.os.ui.render.viewer.editor.lsp(
   new, enabled_by_default, command_for, status, start, stop, restart, toggle_popups,
   did_open, did_change, did_save, completion, hover, definition,
   diagnostics_text, diagnostics_from_check, output_name
)

use std.core
use std.core.str as str
use std.os (env)
use std.os.path as ospath
use std.os.ui.render.viewer.editor.tools as tools
use std.parse.data.json (json_encode)

fn _ext_key(str path) str {
   def ext = str.upper(str.str_replace(ospath.extname(path), ".", ""))
   ext.len > 0 ? ext : "TEXT"
}

fn _argv_from_env(str key, str path) list {
   def raw = str.strip(env(key))
   if(raw.len <= 0){ return [] }
   mut out = []
   def parts = str.split_words(raw)
   mut i = 0
   while(i < parts.len){
      def w = to_str(parts.get(i, ""))
      out = out.append(w == "{file}" ? path : w)
      i += 1
   }
   out
}

fn enabled_by_default() bool {
   def raw = str.lower(str.strip(env("NY_EDITOR_LSP")))
   !(raw == "0" || raw == "false" || raw == "off" || raw == "no")
}

fn command_for(str path) list {
   if(!enabled_by_default()){ return [] }
   def per_ext = _argv_from_env("NY_EDITOR_LSP_" + _ext_key(path), path)
   if(per_ext.len > 0){ return per_ext }
   def generic = _argv_from_env("NY_EDITOR_LSP_COMMAND", path)
   if(generic.len > 0){ return generic }
   [tools.lsp_command()]
}

fn new() dict {
   {
      "active": false, "popups": true, "request_id": 1, "lang": "",
      "path": "", "uri": "", "argv": [], "diagnostics": [], "completions": [],
      "completion_index": 0, "hover": "", "definition": dict(8), "last": ""
   }
}

fn _uri(str path) str {
   if(str.startswith(path, "file://")){ return path }
   "file://" + ospath.normalize(path)
}

fn _lang(str path) str {
   def ext = str.lower(ospath.extname(path))
   if(ext == ".ny"){ return "nytrix" }
   if(ext == ".c" || ext == ".h"){ return "c" }
   if(ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".cxx"){ return "cpp" }
   if(ext == ".py"){ return "python" }
   if(ext == ".js"){ return "javascript" }
   if(ext == ".ts"){ return "typescript" }
   "plaintext"
}

fn _next_id(dict st) list {
   def id = int(st.get("request_id", 1))
   st["request_id"] = id + 1
   [st, id]
}

fn _rpc(any body) str {
   def json = json_encode(body)
   "Content-Length: " + to_str(json.len) + "\r\n\r\n" + json
}

fn _doc(str uri) dict { {"uri": uri} }

fn start(dict st, str path, str text="") dict {
   def argv = command_for(path)
   if(argv.len <= 0){
      st["active"] = false
      st["last"] = "no language server configured"
      return st
   }
   st["active"] = true
   st["path"] = path
   st["uri"] = _uri(path)
   st["lang"] = _lang(path)
   st["argv"] = argv
   st["last"] = "lsp ready: " + str.join(argv, " ")
   if(text.len > 0){ st["last_request"] = did_open(st, path, text) }
   st
}

fn stop(dict st) dict {
   st["active"] = false
   st["last"] = "lsp stopped"
   st
}

fn restart(dict st, str path, str text="") dict {
   start(stop(st), path, text)
}

fn toggle_popups(dict st) dict {
   st["popups"] = !bool(st.get("popups", true))
   st["last"] = bool(st.get("popups", true)) ? "lsp popups on" : "lsp popups off"
   st
}

fn status(dict st) str {
   def argv = st.get("argv", command_for(to_str(st.get("path", ""))))
   (st.get("active", false) ? "active" : "idle") +
   " popups=" + (st.get("popups", true) ? "on" : "off") +
   " cmd=" + (argv.len > 0 ? str.join(argv, " ") : "none")
}

fn did_open(dict st, str path, str text) str {
   def uri = _uri(path)
   _rpc({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": _lang(path), "version": 1, "text": text}}})
}

fn did_change(dict st, str path, str text) str {
   def uri = _uri(path)
   def id_pair = _next_id(st)
   st = id_pair.get(0)
   _rpc({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": uri, "version": int(id_pair.get(1, 1))}, "contentChanges": [{"text": text}]}})
}

fn did_save(dict st, str path) str {
   _rpc({"jsonrpc": "2.0", "method": "textDocument/didSave", "params": {"textDocument": _doc(_uri(path))}})
}

fn _pos(int line, int col) dict {
   {"line": max(0, line), "character": max(0, col)}
}

fn completion(dict st, str path, int line, int col) dict {
   def id_pair = _next_id(st)
   st = id_pair.get(0)
   st["last"] = "completion requested"
   st["last_request"] = _rpc({"jsonrpc": "2.0", "id": int(id_pair.get(1, 1)), "method": "textDocument/completion", "params": {"textDocument": _doc(_uri(path)), "position": _pos(line, col), "context": {"triggerKind": 1}}})
   st
}

fn hover(dict st, str path, int line, int col) dict {
   def id_pair = _next_id(st)
   st = id_pair.get(0)
   st["last"] = "hover requested"
   st["last_request"] = _rpc({"jsonrpc": "2.0", "id": int(id_pair.get(1, 1)), "method": "textDocument/hover", "params": {"textDocument": _doc(_uri(path)), "position": _pos(line, col)}})
   st
}

fn definition(dict st, str path, int line, int col) dict {
   def id_pair = _next_id(st)
   st = id_pair.get(0)
   st["last"] = "definition requested"
   st["last_request"] = _rpc({"jsonrpc": "2.0", "id": int(id_pair.get(1, 1)), "method": "textDocument/definition", "params": {"textDocument": _doc(_uri(path)), "position": _pos(line, col)}})
   st
}

fn diagnostics_from_check(dict check_res) list {
   def text = to_str(check_res.get("stdout", ""))
   def lines = str.split(text, "\n")
   mut out = []
   mut i = 0
   while(i < lines.len){
      def line = to_str(lines.get(i, ""))
      if(str.str_contains(line, "[E") || str.str_contains(line, "[W")){
         out = out.append({"line": line, "severity": str.str_contains(line, "[E") ? 1 : 2, "message": line})
      }
      i += 1
   }
   out
}

fn diagnostics_text(dict st) str {
   def ds = st.get("diagnostics", [])
   if(ds.len <= 0){ return "No diagnostics\n" + status(st) }
   mut out = []
   mut i = 0
   while(i < ds.len){
      def d = ds.get(i, {})
      out = out.append(to_str(i + 1) + ". " + to_str(d.get("message", d.get("line", ""))))
      i += 1
   }
   str.join(out, "\n")
}

fn output_name(str path) str {
   "*lsp: " + (path.len > 0 ? ospath.basename(path) : "status") + "*"
}

#main {
   mut st = start(new(), "main.ny", "use std.core\n")
   st = completion(st, "main.ny", 0, 1)
   assert(str.str_contains(to_str(st.get("last_request", "")), "textDocument/completion"), "lsp request")
   assert(status(st).len > 0, "lsp status")
}
