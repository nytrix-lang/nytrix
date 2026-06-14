;; Keywords: subprocess process spawn os
;; Subprocess convenience API for quick scripts that need spawn, capture, and wait behavior.
;; References:
;; - std.os
module std.os.subprocess(check_output, output, check_lines, shell, shell_lines, run_capture)
use std.core
use std.core.error
use std.core.str as str
use std.os.io as pio

fn _cmd_args(any cmd, any args=[]) list {
   if is_list(cmd) { return cmd }
   mut out = []
   out = out.append(to_str(cmd))
   if is_list(args) {
      mut i = 0
      while i < args.len {
         out = out.append(to_str(args.get(i)))
         i += 1
      }
   }
   out
}

fn run_capture(any cmd, any args=[], any input=nil, bool check=true) dict {
   "Runs a command and captures stdout. Returns `{code, stdout, ok, argv}`.
   `cmd` may be a string plus `args`, or a full argv list like `['git', 'status']`."
   def argv = _cmd_args(cmd, args)
   def path = to_str(argv.get(0, ""))
   def p = pio.spawn(path, argv)
   if p == 0 {
      if check { panic("spawn failed: " + repr(argv)) }
      mut fail = dict(4)
      fail["code"] = 127
      fail["stdout"] = ""
      fail["ok"] = false
      fail["argv"] = argv
      return fail
   }
   if input != nil && input != 0 {
      def _ignored_send = pio.send(p, input)
      _ignored_send
   }
   def _ignored_shutdown = pio.shutdown_send(p)
   _ignored_shutdown
   def stdout = pio.recv_all(p, 4096)
   def status = pio.close(p)
   mut code = 1
   if is_ok(status) { code = unwrap(status) }
   mut out = dict(8)
   out["code"] = code
   out["stdout"] = stdout
   out["ok"] = code == 0
   out["argv"] = argv
   if check && code != 0 { panic("command failed(" + to_str(code) + "): " + repr(argv)) }
   out
}

fn check_output(any cmd, any args=[], bool text=true, bool strip=false, any input=nil) str {
   "Python-style check_output. Returns stdout or panics on non-zero exit."
   def res = run_capture(cmd, args, input, true)
   mut out = res.get("stdout", "")
   if strip { out = str.strip(out) }
   out
}

fn output(any cmd, any args=[], bool strip=false) str {
   "Short alias for `check_output`."
   check_output(cmd, args, true, strip)
}

fn _split_lines(str text, bool keep_empty=false) list {
   mut raw = str.split(text, "\n")
   mut out = []
   mut i = 0
   while i < raw.len {
      mut line = raw.get(i, "")
      if str.endswith(line, "\r") { line = slice(line, 0, line.len - 1) }
      if keep_empty || line.len > 0 || i + 1 < raw.len { out = out.append(line) }
      i += 1
   }
   out
}

fn check_lines(any cmd, any args=[], bool keep_empty=false, any input=nil) list {
   "Runs a command and returns stdout split into lines."
   _split_lines(check_output(cmd, args, true, false, input), keep_empty)
}

fn shell(str command, bool check=true, bool strip=false) str {
   "Runs a shell command and returns stdout. Prefer argv-list commands for untrusted data."
   mut argv = ["/bin/sh", "-c", command]
   #windows {
      argv = ["cmd", "/c", command]
   } #endif
   def res = run_capture(argv, [], nil, check)
   mut out = res.get("stdout", "")
   if strip { out = str.strip(out) }
   out
}

fn shell_lines(str command, bool keep_empty=false) list {
   "Runs a shell command and returns stdout lines."
   _split_lines(shell(command, true, false), keep_empty)
}

#main {
   mut echo_argv = ["/bin/sh", "-c", "echo subprocess-probe"]
   #windows {
      echo_argv = ["cmd", "/c", "echo subprocess-probe"]
   } #endif
   def cap = run_capture(echo_argv, [], nil, true)
   assert(cap.get("ok", false) && str.strip(cap.get("stdout", "")) == "subprocess-probe", "subprocess run_capture")
   assert(check_output(echo_argv, [], true, true) == "subprocess-probe", "subprocess check_output")
   assert(output(echo_argv, [], true) == "subprocess-probe", "subprocess output")
   assert(check_lines(echo_argv) == ["subprocess-probe"], "subprocess check_lines")
   assert(shell("echo shell-probe", true, true) == "shell-probe", "subprocess shell")
   assert(shell_lines("echo line-probe") == ["line-probe"], "subprocess shell_lines")
   print("✓ std.os.subprocess self-test passed")
}
