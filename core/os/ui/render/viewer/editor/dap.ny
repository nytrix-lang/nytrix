;; Keywords: editor dap debugger breakpoints stepping stack variables debug-adapter
;; Debug Adapter Protocol client for the editor. `launch` starts the adapter
;; (ny-dap by default: it compiles .ny programs with debug info and fronts
;; GDB's DAP interpreter or lldb-dap), performs the initialize/launch/
;; configurationDone handshake and installs breakpoints. Call `pump` once per
;; frame: it answers adapter requests, chains stopped -> stackTrace -> scopes
;; -> variables, and returns events (stopped, continued, output, exited,
;; terminated, frames, variables, evaluate, breakpoints, error).
;; Lines are 1-based on the wire and 0-based in this API.
;; References:
;; - core.os.jsonrpc
;; - https://microsoft.github.io/debug-adapter-protocol/specification
module core.os.ui.render.viewer.editor.dap(
   new, command_for, launch, stop, running, stopped, status, state_text,
   toggle_breakpoint, set_breakpoint, clear_breakpoints, breakpoints_for, has_breakpoint,
   continue_run, pause, step_over, step_in, step_out, restart_frame,
   stack, select_frame, variables, expand, evaluate, pump, output_text, console_text, stop_location
)

use core
use core.str as str
use core.math (max, min)
use core.os (env, getcwd, file_exists)
use core.os.path as ospath
use core.os.jsonrpc as rpc

fn _as_list(any v) list { is_list(v) ? v : [] }

fn _norm(str path) str {
   if path.len <= 0 { return "" }
   def p = str.startswith(path, "/") ? path : getcwd() + "/" + path
   ospath.normalize(p)
}

fn command_for(str path) list {
   "Adapter command: NY_EDITOR_DAP_ADAPTER(words, {file} = path), else ny-dap."
   def raw = str.strip(env("NY_EDITOR_DAP_ADAPTER"))
   if raw.len > 0 {
      mut out = []
      for w in str.split_words(raw) { out = out.append(to_str(w) == "{file}" ? path : to_str(w)) }
      return out
   }
   def local = "./build/release/ny-dap"
   [file_exists(local) ? _norm(local) : "ny-dap"]
}

fn new() dict {
   "Creates an idle debugger state."
   {
      "conn": nil, "seq": 1, "pending": dict(8), "phase": "idle", "program": "",
      "breakpoints": dict(8), "verified": dict(8), "thread": 0, "frames": [],
      "frame": 0, "scopes": [], "vars": dict(8), "output": "", "last": "debugger idle",
      "stop_reason": "", "caps": dict(0), "exit_code": -1, "launch_args": dict(0)
   }
}

fn running(dict st) bool {
   "True while a debug session is alive(running or stopped)."
   def c = st.get("conn", nil)
   is_dict(c) && rpc.alive(c) && to_str(st.get("phase", "idle")) != "idle"
}

fn stopped(dict st) bool { to_str(st.get("phase", "")) == "stopped" }

fn status(dict st) str { to_str(st.get("last", "")) }

fn _send(dict st, str command, any args, str kind, dict extra=dict(0)) int {
   def c = st.get("conn", nil)
   if !is_dict(c) { return 0 }
   def int seq = int(st.get("seq", 1))
   st["seq"] = seq + 1
   mut dict info = {"kind": kind, "command": command}
   for k in extra.keys() { info[k] = extra.get(k) }
   st.get("pending", dict(8))[to_str(seq)] = info
   mut dict msg = {"seq": seq, "type": "request", "command": command}
   if !is_nil(args) { msg["arguments"] = args }
   rpc.send(c, msg) ? seq : 0
}

fn _reply(dict st, dict req, bool ok=true, any body=nil) bool {
   def c = st.get("conn", nil)
   if !is_dict(c) { return false }
   def int seq = int(st.get("seq", 1))
   st["seq"] = seq + 1
   mut dict msg = {"seq": seq, "type": "response", "request_seq": req.get("seq", 0), "success": ok, "command": to_str(req.get("command", ""))}
   if !is_nil(body) { msg["body"] = body }
   rpc.send(c, msg)
}

fn launch(dict st, str program, list args=[], str cwd="", bool stop_on_entry=false) dict {
   "Starts the adapter and launches `program` (a .ny/.nshape file or an
   executable) with the current breakpoints."
   if running(st) { st = stop(st) }
   def path = _norm(program)
   def dir = cwd.len > 0 ? cwd : getcwd()
   def conn = rpc.start(command_for(path), dir)
   if !is_dict(conn) {
      st["last"] = "debugger: " + to_str(conn)
      return st
   }
   st["conn"] = conn
   st["seq"] = 1
   st["pending"] = dict(8)
   st["phase"] = "initializing"
   st["program"] = path
   st["frames"] = []
   st["scopes"] = []
   st["vars"] = dict(8)
   st["output"] = ""
   st["console"] = ""
   st["exit_code"] = -1
   st["launch_args"] = {"program": path, "args": args, "cwd": dir, "stopOnEntry": stop_on_entry, "stopAtBeginningOfMainSubprogram": stop_on_entry}
   _send(st, "initialize", {
         "clientID": "nytrix-editor", "clientName": "Nytrix editor", "adapterID": "nytrix",
         "linesStartAt1": true, "columnsStartAt1": true, "pathFormat": "path",
         "supportsVariableType": true, "supportsRunInTerminalRequest": false
      }, "initialize")
   st["last"] = "debugger starting " + ospath.basename(path)
   st
}

fn stop(dict st) dict {
   "Ends the session and terminates the debuggee."
   def c = st.get("conn", nil)
   if is_dict(c) {
      if rpc.alive(c) { _send(st, "disconnect", {"terminateDebuggee": true}, "disconnect") }
      rpc.stop(c)
   }
   st["conn"] = nil
   st["phase"] = "idle"
   st["frames"] = []
   st["scopes"] = []
   st["vars"] = dict(8)
   st["last"] = "debugger stopped"
   st
}

;; ---- breakpoints ---------------------------------------------------------
fn breakpoints_for(dict st, str path) list {
   "0-based breakpoint lines of `path`, ascending."
   _as_list(st.get("breakpoints", dict(0)).get(_norm(path), []))
}

fn has_breakpoint(dict st, str path, int line) bool {
   breakpoints_for(st, path).contains(line)
}

fn _set_lines(dict st, str path, list lines) dict {
   def key = _norm(path)
   mut sorted_lines = sorted(lines)
   st.get("breakpoints", dict(8))[key] = sorted_lines
   if running(st) && to_str(st.get("phase", "")) != "initializing" { _send_breakpoints(st, key) }
   st
}

fn set_breakpoint(dict st, str path, int line, bool on=true) dict {
   "Adds or removes a breakpoint(live sessions are updated immediately)."
   mut lines = breakpoints_for(st, path)
   if on && !lines.contains(line) { lines = lines.append(line) }
   if !on {
      mut keep = []
      for l in lines { if int(l) != line { keep = keep.append(l) } }
      lines = keep
   }
   _set_lines(st, path, lines)
}

fn toggle_breakpoint(dict st, str path, int line) dict {
   "Toggles the breakpoint on a 0-based line."
   set_breakpoint(st, path, line, !has_breakpoint(st, path, line))
}

fn clear_breakpoints(dict st) dict {
   "Removes every breakpoint."
   def keys = st.get("breakpoints", dict(0)).keys()
   for k in keys { _set_lines(st, to_str(k), []) }
   st
}

fn _send_breakpoints(dict st, str path) int {
   mut bps = []
   for l in breakpoints_for(st, path) { bps = bps.append({"line": int(l) + 1}) }
   _send(st, "setBreakpoints", {"source": {"path": path, "name": ospath.basename(path)}, "breakpoints": bps, "sourceModified": false}, "breakpoints", {"path": path})
}

;; ---- run control -----------------------------------------------------------
fn _thread_args(dict st) dict { {"threadId": int(st.get("thread", 1))} }

fn _resume(dict st, str command) dict {
   if !stopped(st) { st["last"] = "not stopped" return st }
   _send(st, command, _thread_args(st), "resume")
   st["phase"] = "running"
   st["frames"] = []
   st["scopes"] = []
   st["last"] = command
   st
}

fn continue_run(dict st) dict { "Resumes execution." _resume(st, "continue") }

fn step_over(dict st) dict { "Steps over one line." _resume(st, "next") }

fn step_in(dict st) dict { "Steps into a call." _resume(st, "stepIn") }

fn step_out(dict st) dict { "Runs until the current function returns." _resume(st, "stepOut") }

fn restart_frame(dict st) dict {
   "Restarts the selected frame when the adapter supports it."
   def frame = st.get("frames", []).get(int(st.get("frame", 0)), dict(0))
   if is_dict(frame) && frame.contains("id") { _send(st, "restartFrame", {"frameId": frame.get("id")}, "resume") }
   st
}

fn pause(dict st) dict {
   "Interrupts a running program."
   if to_str(st.get("phase", "")) == "running" { _send(st, "pause", {"threadId": int(max(1, int(st.get("thread", 1))))}, "pause") }
   st
}

;; ---- inspection ------------------------------------------------------------
fn stack(dict st) list {
   "Frames of the stopped thread: {id, name, path, line, col} (0-based line)."
   _as_list(st.get("frames", []))
}

fn stop_location(dict st) dict {
   "Path and 0-based line of the selected frame, or an empty dict."
   def frames = stack(st)
   if frames.len <= 0 { return dict(0) }
   frames.get(int(min(max(int(st.get("frame", 0)), 0), frames.len - 1)), dict(0))
}

fn select_frame(dict st, int index) dict {
   "Selects a frame and loads its scopes and variables."
   def frames = stack(st)
   if index < 0 || index >= frames.len { return st }
   st["frame"] = index
   st["scopes"] = []
   _send(st, "scopes", {"frameId": frames.get(index, dict(0)).get("id", 0)}, "scopes")
   st
}

fn variables(dict st) list {
   "Scopes of the selected frame, each {name, ref, expensive, vars}."
   mut out = []
   def vars = st.get("vars", dict(0))
   for s in st.get("scopes", []) {
      mut dict row = s
      row["vars"] = _as_list(vars.get(to_str(s.get("ref", 0)), []))
      out = out.append(row)
   }
   out
}

fn expand(dict st, int ref) dict {
   "Loads the children of a structured variable(its `ref`)."
   if ref > 0 { _send(st, "variables", {"variablesReference": ref}, "variables", {"ref": ref}) }
   st
}

fn evaluate(dict st, str expr, str context="repl") dict {
   "Evaluates `expr` in the selected frame; answers as an `evaluate` event."
   def f = stop_location(st)
   mut dict args = {"expression": expr, "context": context}
   if f.contains("id") { args["frameId"] = f.get("id") }
   _send(st, "evaluate", args, "evaluate", {"expr": expr})
   st
}

fn output_text(dict st) str { "Program output(stdout/stderr)." to_str(st.get("output", "")) }

fn console_text(dict st) str { "Adapter and debugger messages." to_str(st.get("console", "")) }

fn state_text(dict st) str {
   "Readable dump of the session: stack, then variables of the selected frame."
   mut lines = [status(st)]
   def frames = stack(st)
   if frames.len > 0 {
      lines = lines.append("")
      lines = lines.append("Call stack")
      mut int i = 0
      while i < frames.len {
         def f = frames.get(i, dict(0))
         def mark = i == int(st.get("frame", 0)) ? "> " : "  "
         lines = lines.append(mark + to_str(f.get("name", "?")) + "  " + ospath.basename(to_str(f.get("path", ""))) + ":" + to_str(int(f.get("line", 0)) + 1))
         i += 1
      }
   }
   for s in variables(st) {
      lines = lines.append("")
      lines = lines.append(to_str(s.get("name", "")))
      for v in s.get("vars", []) {
         def ty = to_str(v.get("type", ""))
         lines = lines.append("  " + to_str(v.get("name", "")) + (ty.len > 0 ? ": " + ty : "") + " = " + to_str(v.get("value", "")))
      }
   }
   str.join(lines, "\n")
}

;; ---- message handling ------------------------------------------------------
fn _frame_name(str raw) str {
   ;; Native symbols carry the internal-call prefix; show the source name.
   str.startswith(raw, "ny_fn_") ? str.str_slice(raw, 6, str.utf8_len(raw)) : raw
}

fn _frames(any body) list {
   mut out = []
   if !is_dict(body) { return out }
   for f in body.get("stackFrames", []) {
      if !is_dict(f) { continue }
      def src = f.get("source", dict(0))
      out = out.append({
            "id": f.get("id", 0), "name": _frame_name(to_str(f.get("name", "?"))),
            "path": is_dict(src) ? to_str(src.get("path", "")) : "",
            "line": int(max(0, int(f.get("line", 1)) - 1)), "col": int(max(0, int(f.get("column", 1)) - 1))
         })
   }
   out
}

fn _vars(any body) list {
   mut out = []
   if !is_dict(body) { return out }
   for v in body.get("variables", []) {
      if !is_dict(v) { continue }
      out = out.append({"name": to_str(v.get("name", "")), "value": to_str(v.get("value", "")), "type": to_str(v.get("type", "")), "ref": int(v.get("variablesReference", 0))})
   }
   out
}

fn _after_configured(dict st) int {
   for p in st.get("breakpoints", dict(0)).keys() { _send_breakpoints(st, to_str(p)) }
   _send(st, "configurationDone", dict(0), "configured")
}

fn _response(dict st, dict m) list {
   def pending = st.get("pending", dict(8))
   def key = to_str(m.get("request_seq", 0))
   if !pending.contains(key) { return [] }
   def info = pending.get(key, dict(0))
   pending.remove(key)
   def kind = to_str(info.get("kind", ""))
   def body = m.get("body", nil)
   if !bool(m.get("success", false)) {
      def msg = to_str(m.get("message", kind + " failed"))
      def detail = is_dict(body) && is_dict(body.get("error", nil)) ? to_str(body.get("error").get("format", "")) : ""
      st["last"] = kind + ": " + (detail.len > 0 ? detail : msg)
      if kind == "launch" || kind == "initialize" { st["phase"] = "idle" }
      return [{"kind": "error", "request": kind, "text": to_str(st.get("last", ""))}]
   }
   if kind == "initialize" {
      st["caps"] = is_dict(body) ? body : dict(0)
      _send(st, "launch", st.get("launch_args", dict(0)), "launch")
      st["last"] = "launching " + ospath.basename(to_str(st.get("program", "")))
      return []
   }
   if kind == "launch" {
      if to_str(st.get("phase", "")) == "initializing" { st["phase"] = "running" }
      st["last"] = "running " + ospath.basename(to_str(st.get("program", "")))
      return [{"kind": "continued"}]
   }
   if kind == "stack" {
      st["frames"] = _frames(body)
      st["frame"] = 0
      def frames = _as_list(st.get("frames", []))
      if frames.len > 0 { _send(st, "scopes", {"frameId": frames.get(0, dict(0)).get("id", 0)}, "scopes") }
      return [{"kind": "frames", "frames": frames}]
   }
   if kind == "scopes" {
      mut scopes = []
      if is_dict(body) {
         for s in body.get("scopes", []) {
            if !is_dict(s) { continue }
            def int ref = int(s.get("variablesReference", 0))
            ;; Register dumps are huge and rarely wanted: load them on demand.
            def expensive = bool(s.get("expensive", false)) || str.lower(to_str(s.get("name", ""))) == "registers"
            scopes = scopes.append({"name": to_str(s.get("name", "")), "ref": ref, "expensive": expensive})
            if ref > 0 && !expensive { _send(st, "variables", {"variablesReference": ref}, "variables", {"ref": ref}) }
         }
      }
      st["scopes"] = scopes
      return []
   }
   if kind == "variables" {
      def int ref = int(info.get("ref", 0))
      st.get("vars", dict(8))[to_str(ref)] = _vars(body)
      return [{"kind": "variables", "ref": ref}]
   }
   if kind == "evaluate" {
      def v = is_dict(body) ? body : dict(0)
      return [{"kind": "evaluate", "expr": to_str(info.get("expr", "")), "value": to_str(v.get("result", "")), "type": to_str(v.get("type", "")), "ref": int(v.get("variablesReference", 0))}]
   }
   if kind == "breakpoints" {
      def path = to_str(info.get("path", ""))
      mut ok = []
      if is_dict(body) { for b in body.get("breakpoints", []) { if is_dict(b) && bool(b.get("verified", false)) { ok = ok.append(int(b.get("line", 0)) - 1) } } }
      st.get("verified", dict(8))[path] = ok
      return [{"kind": "breakpoints", "path": path, "verified": ok}]
   }
   []
}

fn _event(dict st, dict m) list {
   def ev = to_str(m.get("event", ""))
   def body = m.get("body", dict(0))
   if ev == "initialized" {
      _after_configured(st)
      return []
   }
   if ev == "stopped" {
      st["phase"] = "stopped"
      st["thread"] = int(body.get("threadId", st.get("thread", 1)))
      st["stop_reason"] = to_str(body.get("reason", ""))
      st["last"] = "stopped: " + to_str(st.get("stop_reason", "")) + (body.contains("description") ? " (" + to_str(body.get("description", "")) + ")" : "")
      _send(st, "stackTrace", {"threadId": int(st.get("thread", 1)), "startFrame": 0, "levels": 64}, "stack")
      return [{"kind": "stopped", "reason": to_str(st.get("stop_reason", "")), "text": to_str(body.get("text", ""))}]
   }
   if ev == "continued" {
      st["phase"] = "running"
      st["frames"] = []
      return [{"kind": "continued"}]
   }
   if ev == "output" {
      def cat = to_str(body.get("category", "console"))
      def text = to_str(body.get("output", ""))
      ;; Program output (stdout/stderr) and adapter chatter (console) are
      ;; kept apart so a debug console can show the program's own output.
      def key = cat == "stdout" || cat == "stderr" || cat == "important" ? "output" : "console"
      if cat != "telemetry" {
         mut all = to_str(st.get(key, "")) + text
         if all.len > 200000 { all = str.str_slice(all, all.len - 150000, all.len) }
         st[key] = all
      }
      return [{"kind": "output", "category": cat, "text": text}]
   }
   if ev == "exited" {
      st["exit_code"] = int(body.get("exitCode", 0))
      st["last"] = "program exited with code " + to_str(st.get("exit_code", 0))
      return [{"kind": "exited", "code": int(st.get("exit_code", 0))}]
   }
   if ev == "terminated" {
      st["phase"] = "idle"
      st["frames"] = []
      st["scopes"] = []
      if !str.startswith(to_str(st.get("last", "")), "program exited") { st["last"] = "debug session ended" }
      def c = st.get("conn", nil)
      if is_dict(c) { _send(st, "disconnect", {"terminateDebuggee": true}, "disconnect") rpc.stop(c) }
      st["conn"] = nil
      return [{"kind": "terminated"}]
   }
   if ev == "breakpoint" {
      return [{"kind": "breakpoint_changed"}]
   }
   []
}

fn pump(dict st) list {
   "Processes adapter traffic since the last call and returns events."
   def c = st.get("conn", nil)
   if !is_dict(c) { return [] }
   mut out = []
   for m in rpc.poll(c, 0) {
      if !is_dict(m) { continue }
      def t = to_str(m.get("type", ""))
      if t == "response" { out = out.extend(_response(st, m)) }
      elif t == "event" { out = out.extend(_event(st, m)) }
      elif t == "request" {
         ;; Reverse requests (runInTerminal, startDebugging): decline politely.
         _reply(st, m, false)
      }
   }
   if is_dict(st.get("conn", nil)) && !rpc.alive(c) && to_str(st.get("phase", "")) != "idle" {
      st["phase"] = "idle"
      st["last"] = "debug adapter exited"
      out = out.append({"kind": "terminated"})
   }
   out
}

#main {
   mut st = new()
   st = toggle_breakpoint(st, "/tmp/x.ny", 4)
   st = toggle_breakpoint(st, "/tmp/x.ny", 2)
   assert(to_str(breakpoints_for(st, "/tmp/x.ny")) == to_str([2, 4]), "sorted breakpoints")
   st = toggle_breakpoint(st, "/tmp/x.ny", 4)
   assert(to_str(breakpoints_for(st, "/tmp/x.ny")) == to_str([2]), "toggle off")
   assert(!running(st), "idle")
   print("dap client tests passed")
}
