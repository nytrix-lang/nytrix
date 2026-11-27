;; Keywords: editor lsp language-server diagnostics completion hover definition references rename format symbols
;; Language Server Protocol client for editor buffers. `start` spawns the
;; server (ny-lsp by default, NY_EDITOR_LSP_<EXT> or NY_EDITOR_LSP_COMMAND to
;; override) and performs the initialize handshake; document notifications
;; and requests are queued until the server is ready. Call `pump` once per
;; frame: it answers server requests and returns normalized events
;; (diagnostics, completion, hover, definition, references, rename, format,
;; symbols, signature, actions, message). Positions are 0-based lines and
;; code-point columns; `lsp_col`/`editor_col` convert to the server unit.
;; References:
;; - core.os.jsonrpc
;; - https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
module core.os.ui.render.viewer.editor.lsp(
   new, enabled_by_default, command_for, status, start, stop, restart, toggle_popups,
   ready, did_open, did_change, did_save, did_close, completion, hover, definition,
   references, rename, formatting, document_symbols, signature_help, code_actions,
   workspace_symbols, execute_command, inlay_hints, semantic_tokens, decode_inlay_hints, decode_semantic_tokens, merge_semantic_tokens, pump, diagnostics_for, diagnostics_count,
   diagnostics_text, diagnostics_from_check, output_name, uri_of, path_of,
   lsp_col, editor_col, strip_snippet, norm_path
)

use core
use core.str as str
use core.math (max, min)
use core.os (env, getcwd)
use core.os.path as ospath
use core.os.jsonrpc as rpc
use core.os.ui.render.viewer.editor.tools as tools
use core.math.parse.syntax as syntax

fn _as_list(any v) list { is_list(v) ? v : [] }

fn _ext_key(str path) str {
   def ext = str.upper(str.str_replace(ospath.extname(path), ".", ""))
   ext.len > 0 ? ext : "TEXT"
}

fn _argv_from_env(str key, str path) list {
   def raw = str.strip(env(key))
   if raw.len <= 0 { return [] }
   mut out = []
   def parts = str.split_words(raw)
   mut i = 0
   while i < parts.len {
      def w = to_str(parts.get(i, ""))
      out = out.append(w == "{file}" ? path : w)
      i += 1
   }
   out
}

fn enabled_by_default() bool {
   "False when NY_EDITOR_LSP is 0/false/off/no."
   def raw = str.lower(str.strip(env("NY_EDITOR_LSP")))
   !(raw == "0" || raw == "false" || raw == "off" || raw == "no")
}

fn command_for(str path) list {
   "Server command for `path`: NY_EDITOR_LSP_<EXT>, NY_EDITOR_LSP_COMMAND, then
   ny-lsp for Nytrix sources(other languages need an explicit command)."
   if !enabled_by_default() { return [] }
   def per_ext = _argv_from_env("NY_EDITOR_LSP_" + _ext_key(path), path)
   if per_ext.len > 0 { return per_ext }
   def generic = _argv_from_env("NY_EDITOR_LSP_COMMAND", path)
   if generic.len > 0 { return generic }
   def ext = str.lower(ospath.extname(path))
   if ext == ".ny" || ext == ".nshape" || ext == ".nyt" || path.len == 0 { return [tools.lsp_command()] }
   []
}

fn new() dict {
   "Creates an idle client state."
   {
      "active": false, "ready": false, "popups": true, "conn": nil, "argv": [],
      "root": "", "init_id": 0, "caps": dict(8), "encoding": "utf-16",
      "docs": dict(8), "queue": [], "pending": dict(8), "diagnostics": [],
      "diag_by_path": dict(8), "path": "", "uri": "", "lang": "", "last": "lsp idle",
      "server": "", "messages": []
   }
}

fn uri_of(str path) str {
   "file:// URI of `path` (absolute, percent-encoding spaces and %)."
   if str.startswith(path, "file://") { return path }
   mut p = ospath.normalize(path)
   if !str.startswith(p, "/") { p = ospath.normalize(getcwd() + "/" + p) }
   p = str.str_replace(p, "%", "%25")
   "file://" + str.str_replace(p, " ", "%20")
}

fn norm_path(str path) str {
   "Absolute normalized form of `path`, the key diagnostics are stored under."
   path.len > 0 ? path_of(uri_of(path)) : ""
}

fn path_of(str uri) str {
   "Filesystem path of a file:// URI."
   if !str.startswith(uri, "file://") { return uri }
   def p = str.str_slice(uri, 7, uri.len)
   str.str_replace(str.str_replace(p, "%20", " "), "%25", "%")
}

fn _lang(str path) str {
   def ext = str.lower(ospath.extname(path))
   case ext {
      ".ny", ".nyt", ".nshape" -> "nytrix"
      ".c", ".h" -> "c"
      ".cpp", ".hpp", ".cc", ".cxx", ".hh" -> "cpp"
      ".py" -> "python"
      ".js", ".mjs", ".cjs" -> "javascript"
      ".ts", ".tsx" -> "typescript"
      ".rs" -> "rust"
      ".go" -> "go"
      ".lua" -> "lua"
      ".zig" -> "zig"
      ".sh", ".bash" -> "shellscript"
      ".json" -> "json"
      ".md" -> "markdown"
      _ -> "plaintext"
   }
}

fn _conn(dict st) any { st.get("conn", nil) }

fn ready(dict st) bool {
   "True once the server answered initialize."
   bool(st.get("ready", false)) && is_dict(_conn(st)) && rpc.alive(_conn(st))
}

fn _client_caps() dict {
   {
      "general": {"positionEncodings": ["utf-8", "utf-16"]},
      "textDocument": {
         "synchronization": {"didSave": true, "dynamicRegistration": false},
         "completion": {"completionItem": {"snippetSupport": false, "documentationFormat": ["markdown", "plaintext"]}, "contextSupport": true},
         "hover": {"contentFormat": ["markdown", "plaintext"]},
         "signatureHelp": {"signatureInformation": {"documentationFormat": ["markdown", "plaintext"], "parameterInformation": {"labelOffsetSupport": true}}},
         "definition": {"linkSupport": true},
         "references": dict(0),
         "documentSymbol": {"hierarchicalDocumentSymbolSupport": true},
         "formatting": dict(0),
         "rename": {"prepareSupport": true},
         "codeAction": {"codeActionLiteralSupport": {"codeActionKind": {"valueSet": ["quickfix", "refactor", "source"]}}},
         "inlayHint": {"dynamicRegistration": false},
         "semanticTokens": {"dynamicRegistration": false, "requests": {"full": true}, "formats": ["relative"],
            "tokenTypes": ["namespace", "type", "class", "enum", "interface", "struct", "typeParameter", "parameter", "variable", "property", "enumMember", "event", "function", "method", "macro", "keyword", "modifier", "comment", "string", "number", "regexp", "operator", "decorator"],
            "tokenModifiers": ["declaration", "definition", "readonly", "static", "deprecated", "abstract", "async", "modification", "documentation", "defaultLibrary"]},
         "publishDiagnostics": {"relatedInformation": true}
      },
      "workspace": {"applyEdit": true, "workspaceEdit": {"documentChanges": true}, "configuration": true, "symbol": dict(0)},
      "window": {"workDoneProgress": true}
   }
}

fn start(dict st, str path, str text="", str root="") dict {
   "Starts(or reuses) the server for `path` and opens the document."
   def argv = command_for(path)
   if argv.len <= 0 {
      st["last"] = "no language server for " + ospath.basename(path)
      return st
   }
   def c = _conn(st)
   if is_dict(c) && rpc.alive(c) && str.join(argv, " ") == str.join(st.get("argv", []), " ") {
      if path.len > 0 && !st.get("docs", dict(0)).contains(uri_of(path)) { return did_open(st, path, text) }
      return st
   }
   if is_dict(c) { st = stop(st) }
   def dir = root.len > 0 ? root : (path.len > 0 ? ospath.dirname(ospath.normalize(path)) : getcwd())
   ;; The server runs in `dir`; anchor a relative command to our cwd first.
   mut run = argv
   def cmd0 = to_str(argv.get(0, ""))
   if str.find(cmd0, "/") > 0 && !str.startswith(cmd0, "/") {
      run = [ospath.normalize(getcwd() + "/" + cmd0)].extend(argv.slice(1, argv.len))
   }
   def conn = rpc.start(run, dir)
   if !is_dict(conn) {
      st["active"] = false
      st["last"] = "lsp: " + to_str(conn)
      return st
   }
   st["conn"] = conn
   st["argv"] = argv
   st["root"] = dir
   st["active"] = true
   st["ready"] = false
   st["docs"] = dict(8)
   st["queue"] = []
   st["pending"] = dict(8)
   st["init_id"] = rpc.request(conn, "initialize", {
         "processId": nil, "rootUri": uri_of(dir), "rootPath": dir,
         "workspaceFolders": [{"uri": uri_of(dir), "name": ospath.basename(dir)}],
         "capabilities": _client_caps(), "clientInfo": {"name": "nytrix-editor"}
      })
   st["last"] = "lsp starting: " + str.join(argv, " ")
   if path.len > 0 { st = did_open(st, path, text) }
   st
}

fn stop(dict st) dict {
   "Shuts the server down politely, then terminates it."
   def c = _conn(st)
   if is_dict(c) {
      if rpc.alive(c) {
         rpc.request(c, "shutdown")
         rpc.notify(c, "exit")
      }
      rpc.stop(c)
   }
   st["conn"] = nil
   st["active"] = false
   st["ready"] = false
   st["docs"] = dict(8)
   st["pending"] = dict(8)
   st["queue"] = []
   st["last"] = "lsp stopped"
   st
}

fn restart(dict st, str path, str text="") dict {
   "Stops and starts the server for `path`."
   start(stop(st), path, text)
}

fn toggle_popups(dict st) dict {
   "Toggles hover/completion popups."
   st["popups"] = !bool(st.get("popups", true))
   st["last"] = bool(st.get("popups", true)) ? "lsp popups on" : "lsp popups off"
   st
}

fn status(dict st) str {
   "One-line client status."
   def argv = st.get("argv", [])
   def state = ready(st) ? "ready" : (bool(st.get("active", false)) ? "starting" : "idle")
   mut out = "lsp " + state
   if to_str(st.get("server", "")).len > 0 { out = out + " " + to_str(st.get("server", "")) }
   out = out + " docs=" + to_str(st.get("docs", dict(0)).len) + " diagnostics=" + to_str(diagnostics_count(st, ""))
   out + " cmd=" + (argv.len > 0 ? str.join(argv, " ") : "none")
}

;; Queue a message until initialize completes; send directly afterwards.
fn _post(dict st, dict msg) dict {
   def c = _conn(st)
   if !is_dict(c) { return st }
   if !bool(st.get("ready", false)) {
      st["queue"] = st.get("queue", []).append(msg)
      return st
   }
   rpc.send(c, msg)
   st
}

fn _notify(dict st, str method, any params) dict {
   _post(st, {"jsonrpc": "2.0", "method": method, "params": params})
}

fn _request(dict st, str method, any params, str kind, dict extra=dict(0)) dict {
   def c = _conn(st)
   if !is_dict(c) {
      st["last"] = "lsp not running"
      return st
   }
   def int id = int(c.get("next_id", 1))
   c["next_id"] = id + 1
   mut dict info = {"kind": kind}
   for k in extra.keys() { info[k] = extra.get(k) }
   st.get("pending", dict(8))[to_str(id)] = info
   st["last"] = kind + " requested"
   _post(st, {"jsonrpc": "2.0", "id": id, "method": method, "params": params})
}

fn _doc_id(str path) dict { {"uri": uri_of(path)} }

fn _pos(int line, int col) dict { {"line": int(max(0, line)), "character": int(max(0, col))} }

fn did_open(dict st, str path, str text) dict {
   "Opens `path` with `text` on the server(starting it when needed)."
   if path.len <= 0 { return st }
   if !is_dict(_conn(st)) { return start(st, path, text) }
   def uri = uri_of(path)
   st.get("docs", dict(8))[uri] = 1
   st["path"] = path
   st["uri"] = uri
   st["lang"] = _lang(path)
   _notify(st, "textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": _lang(path), "version": 1, "text": text}})
}

fn did_change(dict st, str path, str text) dict {
   "Sends the full new text of `path` (opening it first when needed)."
   if path.len <= 0 || !is_dict(_conn(st)) { return st }
   def uri = uri_of(path)
   def docs = st.get("docs", dict(8))
   if !docs.contains(uri) { return did_open(st, path, text) }
   def int v = int(docs.get(uri, 1)) + 1
   docs[uri] = v
   _notify(st, "textDocument/didChange", {"textDocument": {"uri": uri, "version": v}, "contentChanges": [{"text": text}]})
}

fn did_save(dict st, str path, str text="") dict {
   "Tells the server `path` was saved."
   if path.len <= 0 || !is_dict(_conn(st)) { return st }
   mut dict p = {"textDocument": _doc_id(path)}
   if text.len > 0 { p["text"] = text }
   _notify(st, "textDocument/didSave", p)
}

fn did_close(dict st, str path) dict {
   "Closes `path` on the server."
   if path.len <= 0 || !is_dict(_conn(st)) { return st }
   def uri = uri_of(path)
   def docs = st.get("docs", dict(8))
   if !docs.contains(uri) { return st }
   docs.remove(uri)
   _notify(st, "textDocument/didClose", {"textDocument": {"uri": uri}})
}

fn _at(str path, int line, int col) dict { {"textDocument": _doc_id(path), "position": _pos(line, col)} }

fn completion(dict st, str path, int line, int col, str trigger="") dict {
   "Requests completions at a position; arrives as a `completion` event."
   mut dict p = _at(path, line, col)
   p["context"] = trigger.len > 0 ? {"triggerKind": 2, "triggerCharacter": trigger} : {"triggerKind": 1}
   _request(st, "textDocument/completion", p, "completion", {"path": path, "line": line, "col": col})
}

fn hover(dict st, str path, int line, int col) dict {
   "Requests hover text; arrives as a `hover` event."
   _request(st, "textDocument/hover", _at(path, line, col), "hover", {"path": path, "line": line, "col": col})
}

fn definition(dict st, str path, int line, int col) dict {
   "Requests the definition; arrives as a `definition` event of locations."
   _request(st, "textDocument/definition", _at(path, line, col), "definition", {"path": path})
}

fn references(dict st, str path, int line, int col, bool include_decl=true) dict {
   "Requests references; arrives as a `references` event of locations."
   mut dict p = _at(path, line, col)
   p["context"] = {"includeDeclaration": include_decl}
   _request(st, "textDocument/references", p, "references", {"path": path})
}

fn rename(dict st, str path, int line, int col, str new_name) dict {
   "Requests a rename; arrives as a `rename` event with per-file edits."
   mut dict p = _at(path, line, col)
   p["newName"] = new_name
   _request(st, "textDocument/rename", p, "rename", {"path": path, "name": new_name})
}

fn formatting(dict st, str path, int tab_size=3, bool spaces=true) dict {
   "Requests whole-document formatting; arrives as a `format` event."
   _request(st, "textDocument/formatting", {"textDocument": _doc_id(path), "options": {"tabSize": tab_size, "insertSpaces": spaces}}, "format", {"path": path})
}

fn document_symbols(dict st, str path) dict {
   "Requests the document outline; arrives as a `symbols` event."
   _request(st, "textDocument/documentSymbol", {"textDocument": _doc_id(path)}, "symbols", {"path": path})
}

fn semantic_tokens(dict st, str path) dict {
   "Requests full semantic tokens; responses use the server's advertised legend."
   def version = int(st.get("docs", dict(0)).get(uri_of(path), 0))
   _request(st, "textDocument/semanticTokens/full", {"textDocument": _doc_id(path)}, "semantic_tokens", {"path": path, "version": version})
}

fn inlay_hints(dict st, str path, int start_line, int end_line, int end_col=0) dict {
   "Requests inlay hints in a half-open range, using server position units."
   def int version = int(st.get("docs", dict(0)).get(uri_of(path), 0))
   _request(st, "textDocument/inlayHint", {"textDocument": _doc_id(path), "range": {"start": _pos(start_line, 0), "end": _pos(end_line, end_col)}}, "inlay_hints", {"path": path, "version": version, "start_line": start_line, "end_line": end_line, "end_col": end_col})
}

fn workspace_symbols(dict st, str query) dict {
   "Searches symbols across the workspace; arrives as a `symbols` event."
   _request(st, "workspace/symbol", {"query": query}, "symbols", {"path": "", "query": query})
}

fn signature_help(dict st, str path, int line, int col) dict {
   "Requests call signature help; arrives as a `signature` event."
   _request(st, "textDocument/signatureHelp", _at(path, line, col), "signature", {"path": path, "line": line, "col": col})
}

fn code_actions(dict st, str path, int line, int col, int end_line=-1, int end_col=-1) dict {
   "Requests code actions for a range; arrives as an `actions` event."
   def el = end_line < 0 ? line : end_line
   def ec = end_col < 0 ? col : end_col
   mut diags = []
   for d in diagnostics_for(st, path) {
      if int(d.get("line", -1)) <= el && int(d.get("end_line", -1)) >= line { diags = diags.append(d.get("raw", dict(0))) }
   }
   _request(st, "textDocument/codeAction", {"textDocument": _doc_id(path), "range": {"start": _pos(line, col), "end": _pos(el, ec)}, "context": {"diagnostics": diags}}, "actions", {"path": path})
}

fn execute_command(dict st, str command, list args=[]) dict {
   "Runs a server command(for code actions that carry one)."
   _request(st, "workspace/executeCommand", {"command": command, "arguments": args}, "command", dict(0))
}

;; ---- column conversion ---------------------------------------------------
;; Editor columns count code points; servers count UTF-8 bytes or (by default)
;; UTF-16 code units.
fn _cp_bytes(int lead) int { lead < 0x80 ? 1 : (lead < 0xE0 ? 2 : (lead < 0xF0 ? 3 : 4)) }

fn lsp_col(dict st, str line_text, int col) int {
   "Converts an editor(code point) column to the server's column unit."
   def utf8 = to_str(st.get("encoding", "utf-16")) == "utf-8"
   mut int i = 0
   mut int cps = 0
   mut int units = 0
   while cps < col && i < line_text.len {
      def int n = _cp_bytes(load8(line_text, i))
      units += utf8 ? n : (n == 4 ? 2 : 1)
      i += n
      cps += 1
   }
   units + int(max(0, col - cps))
}

fn editor_col(dict st, str line_text, int col) int {
   "Converts a server column to an editor(code point) column of `line_text`."
   def utf8 = to_str(st.get("encoding", "utf-16")) == "utf-8"
   mut int i = 0
   mut int cps = 0
   mut int units = 0
   while units < col && i < line_text.len {
      def int n = _cp_bytes(load8(line_text, i))
      units += utf8 ? n : (n == 4 ? 2 : 1)
      i += n
      cps += 1
   }
   cps
}

;; ---- response normalization ----------------------------------------------
fn strip_snippet(str s) str {
   "Turns snippet syntax($1, ${2:name}, ${3|a,b|}) into plain text."
   if str.find(s, "$") < 0 { return s }
   mut out = ""
   mut int i = 0
   while i < s.len {
      def c = load8(s, i)
      if c == 92 && i + 1 < s.len { out = out + chr(load8(s, i + 1)) i += 2 continue }
      if c == 36 && i + 1 < s.len {
         def n = load8(s, i + 1)
         if n >= 48 && n <= 57 {
            i += 1
            while i < s.len && load8(s, i) >= 48 && load8(s, i) <= 57 { i += 1 }
            continue
         }
         if n == 123 {
            mut int j = i + 2
            while j < s.len && load8(s, j) >= 48 && load8(s, j) <= 57 { j += 1 }
            mut int depth = 1
            mut int k = j
            while k < s.len && depth > 0 {
               if load8(s, k) == 123 { depth += 1 }
               elif load8(s, k) == 125 { depth -= 1 }
               if depth > 0 { k += 1 }
            }
            mut inner = j < s.len && (load8(s, j) == 58 || load8(s, j) == 124) ? str.str_slice(s, j + 1, k) : ""
            if j < s.len && load8(s, j) == 124 {
               def bar = str.find(inner, ",")
               inner = bar >= 0 ? str.str_slice(inner, 0, bar) : str.str_replace(inner, "|", "")
            }
            out = out + strip_snippet(inner)
            i = k + 1
            continue
         }
      }
      out = out + chr(c)
      i += 1
   }
   out
}

fn _markup(any v) str {
   if is_nil(v) { return "" }
   if is_str(v) { return v }
   if is_list(v) {
      mut parts = []
      for x in v { def t = _markup(x) if t.len > 0 { parts = parts.append(t) } }
      return str.join(parts, "\n\n")
   }
   if is_dict(v) {
      if v.contains("language") { return "```" + to_str(v.get("language", "")) + "\n" + to_str(v.get("value", "")) + "\n```" }
      return to_str(v.get("value", ""))
   }
   to_str(v)
}

fn _kind_name(int k) str {
   def names = ["", "text", "method", "function", "constructor", "field", "variable", "class", "interface", "module", "property", "unit", "value", "enum", "keyword", "snippet", "color", "file", "reference", "folder", "enum-member", "constant", "struct", "event", "operator", "type-parameter"]
   k > 0 && k < names.len ? to_str(names.get(k, "")) : "symbol"
}

fn _symbol_kind(int k) str {
   def names = ["", "file", "module", "namespace", "package", "class", "method", "property", "field", "constructor", "enum", "interface", "function", "variable", "constant", "string", "number", "boolean", "array", "object", "key", "null", "enum-member", "struct", "event", "operator", "type-parameter"]
   k > 0 && k < names.len ? to_str(names.get(k, "")) : "symbol"
}

fn _range(any r) dict {
   def s = is_dict(r) ? r.get("start", dict(0)) : dict(0)
   def e = is_dict(r) ? r.get("end", s) : dict(0)
   {"line": int(s.get("line", 0)), "col": int(s.get("character", 0)), "end_line": int(e.get("line", 0)), "end_col": int(e.get("character", 0))}
}

fn _location(any loc) dict {
   if !is_dict(loc) { return dict(0) }
   ;; Location or LocationLink
   def uri = to_str(loc.get("uri", loc.get("targetUri", "")))
   mut dict out = _range(loc.contains("range") ? loc.get("range") : loc.get("targetSelectionRange", loc.get("targetRange", dict(0))))
   out["uri"] = uri
   out["path"] = path_of(uri)
   out
}

fn _locations(any res) list {
   if is_nil(res) { return [] }
   if is_dict(res) { return [_location(res)] }
   mut out = []
   if is_list(res) { for l in res { def x = _location(l) if x.len > 0 { out = out.append(x) } } }
   out
}

fn _completion_items(any res) list {
   def items = is_dict(res) ? res.get("items", []) : (is_list(res) ? res : [])
   mut out = []
   for it in items {
      if !is_dict(it) { continue }
      def label = to_str(it.get("label", ""))
      mut insert = to_str(it.get("insertText", label))
      mut dict edit = dict(0)
      def te = it.get("textEdit", nil)
      if is_dict(te) {
         insert = to_str(te.get("newText", insert))
         edit = _range(te.contains("range") ? te.get("range") : te.get("replace", te.get("insert", dict(0))))
      }
      if int(it.get("insertTextFormat", 1)) == 2 { insert = strip_snippet(insert) }
      def detail = to_str(it.get("detail", ""))
      out = out.append({
            "label": label, "detail": detail, "kind": _kind_name(int(it.get("kind", 0))),
            "insert": insert, "doc": _markup(it.get("documentation", nil)),
            "sort": to_str(it.get("sortText", label)), "filter": to_str(it.get("filterText", label)),
            "edit": edit
         })
   }
   out
}

fn _text_edits(any edits) list {
   mut out = []
   if !is_list(edits) { return out }
   for e in edits {
      if !is_dict(e) { continue }
      mut dict x = _range(e.get("range", dict(0)))
      x["text"] = to_str(e.get("newText", ""))
      out = out.append(x)
   }
   out
}

fn _workspace_edit(any we) dict {
   ;; {path: [edit...]}, from `changes` or `documentChanges`.
   mut dict out = dict(8)
   if !is_dict(we) { return out }
   def changes = we.get("changes", nil)
   if is_dict(changes) { for uri in changes.keys() { out[path_of(to_str(uri))] = _text_edits(changes.get(uri)) } }
   def docs = we.get("documentChanges", nil)
   if is_list(docs) {
      for d in docs {
         if !is_dict(d) || !d.contains("edits") { continue }
         def p = path_of(to_str(d.get("textDocument", dict(0)).get("uri", "")))
         out[p] = out.get(p, []).extend(_text_edits(d.get("edits", [])))
      }
   }
   out
}

fn _symbols(any res, int depth, list acc) list {
   mut out = acc
   if !is_list(res) { return out }
   for s in res {
      if !is_dict(s) { continue }
      def r = s.contains("selectionRange") ? s.get("selectionRange") : (s.contains("location") ? s.get("location", dict(0)).get("range", dict(0)) : s.get("range", dict(0)))
      mut dict x = _range(r)
      x["name"] = to_str(s.get("name", ""))
      x["kind"] = _symbol_kind(int(s.get("kind", 0)))
      x["detail"] = to_str(s.get("detail", s.get("containerName", "")))
      x["depth"] = depth
      if s.contains("location") { x["path"] = path_of(to_str(s.get("location", dict(0)).get("uri", ""))) }
      out = out.append(x)
      out = _symbols(s.get("children", []), depth + 1, out)
   }
   out
}

fn _signature(any res) dict {
   if !is_dict(res) { return dict(0) }
   def sigs = res.get("signatures", [])
   if !is_list(sigs) || sigs.len <= 0 { return dict(0) }
   def int active = int(res.get("activeSignature", 0))
   def sig = sigs.get(int(min(max(active, 0), sigs.len - 1)), dict(0))
   def label = to_str(sig.get("label", ""))
   def int ap = int(res.get("activeParameter", sig.get("activeParameter", 0)))
   def params = sig.get("parameters", [])
   mut int ps = -1
   mut int pe = -1
   if is_list(params) && ap >= 0 && ap < params.len {
      def pl = params.get(ap, dict(0)).get("label", "")
      if is_list(pl) { ps = int(pl.get(0, -1)) pe = int(pl.get(1, -1)) }
      elif is_str(pl) { ps = str.find(label, pl) pe = ps >= 0 ? ps + pl.len : -1 }
   }
   {"label": label, "doc": _markup(sig.get("documentation", nil)), "param_start": ps, "param_end": pe, "active": ap, "count": sigs.len}
}

fn _diagnostics(any items) list {
   mut out = []
   if !is_list(items) { return out }
   for d in items {
      if !is_dict(d) { continue }
      mut dict x = _range(d.get("range", dict(0)))
      x["severity"] = int(d.get("severity", 1))
      x["message"] = to_str(d.get("message", ""))
      x["source"] = to_str(d.get("source", ""))
      x["code"] = to_str(d.get("code", ""))
      x["raw"] = d
      out = out.append(x)
   }
   out
}

fn _actions(any res) list {
   mut out = []
   if !is_list(res) { return out }
   for a in res {
      if !is_dict(a) { continue }
      mut dict x = {"title": to_str(a.get("title", "")), "kind": to_str(a.get("kind", "")), "edit": _workspace_edit(a.get("edit", nil)), "command": nil}
      def cmd = a.get("command", nil)
      if is_dict(cmd) { x["command"] = cmd }
      elif is_str(cmd) { x["command"] = {"command": cmd, "arguments": a.get("arguments", [])} }
      out = out.append(x)
   }
   out
}

fn decode_inlay_hints(any res) list {
   "Normalizes hint labels and positions, retaining original hint data."
   mut out = []
   if !is_list(res) { return out }
   for hint in res {
      if !is_dict(hint) { continue }
      def pos = hint.get("position", dict(0))
      def any label = hint.get("label", nil)
      mut text = ""
      if is_list(label) { for part in label { if is_dict(part) { text = text + to_str(part.get("value", "")) } } }
      elif is_str(label) { text = to_str(label) }
      if text.len == 0 { continue }
      mut dict item = dict(8)
      item["line"] = int(pos.get("line", 0))
      item["col"] = int(pos.get("character", 0))
      item["label"] = text
      item["kind"] = int(hint.get("kind", 0))
      item["padding_left"] = bool(hint.get("paddingLeft", false))
      item["padding_right"] = bool(hint.get("paddingRight", false))
      item["raw"] = hint
      out = out.append(item)
   }
   out
}

fn decode_semantic_tokens(dict st, any res) list {
   "Decodes full relative semantic token data using the server legend."
   mut out = []
   if !is_dict(res) { return out }
   def data = res.get("data", [])
   if !is_list(data) || data.len % 5 != 0 { return out }
   def provider = st.get("caps", dict(0)).get("semanticTokensProvider", dict(0))
   def legend = is_dict(provider) ? provider.get("legend", dict(0)) : dict(0)
   def types = legend.get("tokenTypes", [])
   def modifiers = legend.get("tokenModifiers", [])
   mut int line = 0
   mut int col = 0
   mut int i = 0
   while i < data.len {
      def int dl = int(data.get(i))
      def int dc = int(data.get(i + 1))
      def int len = int(data.get(i + 2))
      def int typ = int(data.get(i + 3))
      def int mask = int(data.get(i + 4))
      if dl < 0 || dc < 0 || len <= 0 || typ < 0 || typ >= types.len || mask < 0 { return [] }
      line += dl
      col = dl == 0 ? col + dc : dc
      mut names = []
      mut int j = 0
      while j < modifiers.len && j < 31 {
         if (mask & (1 << j)) != 0 { names = names.append(to_str(modifiers.get(j))) }
         j += 1
      }
      out = out.append({"line": line, "col": col, "length": len, "type": to_str(types.get(typ)), "modifiers": names})
      i += 5
   }
   out
}

fn _semantic_kind(str typ) int {
   case typ {
      "namespace", "type", "class", "enum", "interface", "struct", "typeParameter" -> syntax.TOK_TYPE
      "parameter" -> syntax.TOK_PARAM
      "variable" -> syntax.TOK_VARIABLE
      "property", "enumMember" -> syntax.TOK_PROPERTY
      "function", "method", "macro" -> syntax.TOK_FUNCTION
      "keyword", "modifier" -> syntax.TOK_KEYWORD
      "comment" -> syntax.TOK_COMMENT
      "string", "regexp" -> syntax.TOK_STRING
      "number" -> syntax.TOK_NUMBER
      "operator" -> syntax.TOK_OPERATOR
      _ -> -1
   }
}

fn merge_semantic_tokens(dict st, str line, list lexical, list semantic) list {
   "Overlays one line's semantic spans on byte-based syntax tokens. LSP columns
   and lengths use the negotiated encoding ; unrecognized types retain lexical colors."
   mut spans = []
   for token in semantic {
      def int kind = _semantic_kind(to_str(token.get("type", "")))
      if kind < 0 { continue }
      def int col = int(token.get("col", 0))
      def int len = int(token.get("length", 0))
      if col < 0 || len <= 0 { continue }
      def int a = editor_col(st, line, col)
      def int b = editor_col(st, line, col + len)
      def int start = str.str_slice(line, 0, a).len
      def int end = str.str_slice(line, 0, b).len
      if end > start { spans = spans.append([start, end, kind]) }
   }
   if spans.len == 0 { return lexical }
   mut out = []
   mut int pos = 0
   mut int lex = 0
   mut int sem = 0
   mut int run_start = 0
   mut int previous = -1
   while pos < line.len {
      while lex < lexical.len && int(lexical.get(lex).get(1, 0)) + int(lexical.get(lex).get(2, 0)) <= pos { lex += 1 }
      while sem < spans.len && int(spans.get(sem).get(1, 0)) <= pos { sem += 1 }
      mut int kind = int(syntax.TOK_TEXT)
      if lex < lexical.len && int(lexical.get(lex).get(1, 0)) <= pos { kind = int(lexical.get(lex).get(0, syntax.TOK_TEXT)) }
      if sem < spans.len && int(spans.get(sem).get(0, 0)) <= pos { kind = int(spans.get(sem).get(2, syntax.TOK_TEXT)) }
      if previous >= 0 && kind != previous { out = out.append([previous, run_start, pos - run_start]) run_start = pos }
      previous = kind
      pos += _cp_bytes(load8(line, pos))
   }
   if previous >= 0 { out = out.append([previous, run_start, line.len - run_start]) }
   out
}

fn _on_initialized(dict st, dict res) dict {
   def caps = res.get("capabilities", dict(0))
   st["caps"] = is_dict(caps) ? caps : dict(0)
   st["encoding"] = to_str(st.get("caps", dict(0)).get("positionEncoding", "utf-16"))
   def info = res.get("serverInfo", nil)
   if is_dict(info) { st["server"] = to_str(info.get("name", "")) + (info.contains("version") ? " " + to_str(info.get("version", "")) : "") }
   st["ready"] = true
   def c = _conn(st)
   rpc.notify(c, "initialized", dict(0))
   for m in st.get("queue", []) { rpc.send(c, m) }
   st["queue"] = []
   st["last"] = "lsp ready" + (to_str(st.get("server", "")).len > 0 ? ": " + to_str(st.get("server", "")) : "")
   st
}

fn _server_request(dict st, dict m) list {
   ;; Answers requests the server sends; returns any events they produce.
   def c = _conn(st)
   def method = to_str(m.get("method", ""))
   def id = m.get("id")
   if method == "workspace/configuration" {
      def items = m.get("params", dict(0)).get("items", [])
      mut res = []
      mut int i = 0
      while i < items.len { res = res.append(nil) i += 1 }
      rpc.respond(c, id, res)
      return []
   }
   if method == "workspace/applyEdit" {
      rpc.respond(c, id, {"applied": true})
      return [{"kind": "apply_edit", "edits": _workspace_edit(m.get("params", dict(0)).get("edit", nil)), "label": to_str(m.get("params", dict(0)).get("label", ""))}]
   }
   if method == "window/showMessageRequest" {
      rpc.respond(c, id, nil)
      return [{"kind": "message", "text": to_str(m.get("params", dict(0)).get("message", "")), "level": int(m.get("params", dict(0)).get("type", 3))}]
   }
   if method == "workspace/workspaceFolders" {
      rpc.respond(c, id, [{"uri": uri_of(to_str(st.get("root", ""))), "name": ospath.basename(to_str(st.get("root", "")))}])
      return []
   }
   ;; window/workDoneProgress/create, client/registerCapability and unknowns.
   rpc.respond(c, id, nil)
   []
}

fn _response(dict st, dict m) list {
   def key = to_str(m.get("id"))
   def int iid = int(st.get("init_id", 0))
   if iid > 0 && to_str(iid) == key {
      st["init_id"] = 0
      if m.contains("error") {
         st["last"] = "lsp initialize failed: " + to_str(m.get("error", dict(0)).get("message", ""))
         return [{"kind": "message", "text": to_str(st.get("last", "")), "level": 1}]
      }
      _on_initialized(st, m.get("result", dict(0)))
      return [{"kind": "ready", "server": to_str(st.get("server", ""))}]
   }
   def pending = st.get("pending", dict(8))
   if !pending.contains(key) { return [] }
   def info = pending.get(key, dict(0))
   pending.remove(key)
   def kind = to_str(info.get("kind", ""))
   if kind == "semantic_tokens" || kind == "inlay_hints" {
      def path = to_str(info.get("path", ""))
      def int version = int(info.get("version", 0))
      if version <= 0 || int(st.get("docs", dict(0)).get(uri_of(path), 0)) != version { return [] }
   }
   if m.contains("error") {
      def msg = to_str(m.get("error", dict(0)).get("message", "request failed"))
      st["last"] = kind + ": " + msg
      return [{"kind": "error", "request": kind, "text": msg}]
   }
   def res = m.get("result", nil)
   mut dict ev = {"kind": kind}
   for k in info.keys() { if k != "kind" { ev[k] = info.get(k) } }
   case kind {
      "completion" -> { ev["items"] = _completion_items(res) ev["incomplete"] = is_dict(res) && bool(res.get("isIncomplete", false)) }
      "hover" -> {
         ev["text"] = is_dict(res) ? _markup(res.get("contents", nil)) : ""
         if is_dict(res) && res.contains("range") { ev["range"] = _range(res.get("range")) }
      }
      "definition", "references" -> { ev["locations"] = _locations(res) }
      "rename" -> { ev["edits"] = _workspace_edit(res) }
      "format" -> { ev["edits"] = _text_edits(res) }
      "symbols" -> { ev["symbols"] = _symbols(res, 0, []) }
      "signature" -> { ev["signature"] = _signature(res) }
      "actions" -> { ev["actions"] = _actions(res) }
      "semantic_tokens" -> { ev["items"] = decode_semantic_tokens(st, res) }
      "inlay_hints" -> { ev["items"] = decode_inlay_hints(res) }
      _ -> { ev["result"] = res }
   }
   st["last"] = kind + " received"
   [ev]
}

fn pump(dict st) list {
   "Processes everything the server sent since the last call and returns
   normalized events. Call once per frame ; never blocks."
   def c = _conn(st)
   if !is_dict(c) { return [] }
   mut events = []
   for m in rpc.poll(c, 0) {
      if !is_dict(m) { continue }
      if m.contains("method") && m.contains("id") { events = events.extend(_server_request(st, m)) continue }
      if m.contains("method") {
         def method = to_str(m.get("method", ""))
         def p = m.get("params", dict(0))
         if method == "textDocument/publishDiagnostics" {
            def path = path_of(to_str(p.get("uri", "")))
            def ds = _diagnostics(p.get("diagnostics", []))
            st.get("diag_by_path", dict(8))[path] = ds
            if path == norm_path(to_str(st.get("path", ""))) { st["diagnostics"] = ds }
            events = events.append({"kind": "diagnostics", "path": path, "items": ds})
         } elif method == "window/showMessage" || method == "window/logMessage" {
            def level = int(p.get("type", 4))
            if method == "window/showMessage" || level <= 2 {
               events = events.append({"kind": "message", "text": to_str(p.get("message", "")), "level": level})
            }
         }
         continue
      }
      if m.contains("id") { events = events.extend(_response(st, m)) }
   }
   if bool(st.get("active", false)) && !rpc.alive(c) {
      st["active"] = false
      st["ready"] = false
      st["last"] = "lsp exited: " + to_str(c.get("error", ""))
      events = events.append({"kind": "message", "text": to_str(st.get("last", "")), "level": 1})
   }
   events
}

;; ---- diagnostics -----------------------------------------------------------
fn diagnostics_for(dict st, str path) list {
   "Server diagnostics last published for `path`."
   _as_list(st.get("diag_by_path", dict(0)).get(norm_path(path), []))
}

fn diagnostics_count(dict st, str path, int severity=0) int {
   "Counts diagnostics for `path` (every file when empty), optionally of one
   severity(1 error, 2 warning, 3 info, 4 hint)."
   mut int n = 0
   def by = st.get("diag_by_path", dict(0))
   for p in by.keys() {
      if path.len > 0 && to_str(p) != norm_path(path) { continue }
      for d in by.get(p, []) { if severity == 0 || int(d.get("severity", 1)) == severity { n += 1 } }
   }
   n
}

fn diagnostics_from_check(dict check_res) list {
   "Diagnostics parsed from `ny --check` output(used without a server)."
   def text = to_str(check_res.get("stdout", "")) + "\n" + to_str(check_res.get("stderr", ""))
   def lines = str.split(text, "\n")
   mut out = []
   mut i = 0
   while i < lines.len {
      def line = to_str(lines.get(i, ""))
      if str.str_contains(line, "[E") || str.str_contains(line, "[W") {
         ;; path:line:col:  [E1234] error: message
         def parts = str.split(line, ":")
         def int ln = int(max(0, str.atoi(to_str(parts.get(1, "1"))) - 1))
         def int col = int(max(0, str.atoi(to_str(parts.get(2, "1"))) - 1))
         out = out.append({"line": ln, "col": col, "end_line": ln, "end_col": col + 1, "severity": str.str_contains(line, "[E") ? 1 : 2, "message": str.strip(line), "source": "ny check", "code": ""})
      }
      i += 1
   }
   out
}

fn diagnostics_text(dict st) str {
   "Readable list of the current file's diagnostics."
   def ds = st.get("diagnostics", [])
   if ds.len <= 0 { return "No diagnostics\n" + status(st) }
   def sev = ["", "error", "warning", "info", "hint"]
   mut out = []
   mut i = 0
   while i < ds.len {
      def d = ds.get(i, dict(0))
      out = out.append(to_str(int(d.get("line", 0)) + 1) + ":" + to_str(int(d.get("col", 0)) + 1) + " " + to_str(sev.get(int(d.get("severity", 1)), "")) + " " + to_str(d.get("message", "")))
      i += 1
   }
   str.join(out, "\n")
}

fn output_name(str path) str {
   "Title of the LSP output buffer for `path`."
   "lsp: " + (path.len > 0 ? ospath.basename(path) : "status")
}

#main {
   assert(strip_snippet("foo(${1:a}, ${2:b})$0") == "foo(a, b)", "snippet")
   assert(strip_snippet("x${1|one,two|}") == "xone", "choice")
   assert(path_of(uri_of("/tmp/a b.ny")) == "/tmp/a b.ny", "uri round trip")
   def st = new()
   st["encoding"] = "utf-16"
   assert(lsp_col(st, "aé𝄞b", 3) == 4, "utf16 col")
   assert(editor_col(st, "aé𝄞b", 4) == 3, "editor col")
   st["encoding"] = "utf-8"
   assert(lsp_col(st, "aé𝄞b", 3) == 7, "utf8 col")
   assert(editor_col(st, "aé𝄞b", 7) == 3, "utf8 editor col")
   st["caps"] = {"semanticTokensProvider": {"legend": {"tokenTypes": ["variable", "function"], "tokenModifiers": ["declaration", "readonly"]}}}
   def tokens = decode_semantic_tokens(st, {"data": [0, 2, 3, 0, 1, 0, 4, 2, 1, 2, 2, 1, 5, 0, 3]})
   assert(tokens.len == 3 && int(tokens.get(1).get("col")) == 6 && int(tokens.get(2).get("line")) == 2 && int(tokens.get(2).get("col")) == 1, "relative semantic positions")
   assert(to_str(tokens.get(1).get("type")) == "function" && tokens.get(2).get("modifiers").len == 2, "semantic legend and modifier bits")
   assert(decode_semantic_tokens(st, {"data": [0, 0, 1, 99, 0]}).len == 0 && decode_semantic_tokens(st, {"data": [1]}).len == 0, "invalid semantic data rejected")
   st["docs"] = {uri_of("/tmp/test.ny"): 2}
   st["pending"] = {"5": {"kind": "semantic_tokens", "path": "/tmp/test.ny", "version": 1}}
   assert(_response(st, {"id": 5, "result": {"data": [0, 0, 1, 0, 0]}}).len == 0, "stale semantic response discarded")
   print("lsp client tests passed")
}
