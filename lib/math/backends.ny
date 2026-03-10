;; Keywords: backends backend dispatch
;; Optional native math backend discovery.
module std.math.backends(backend_mode, native_enabled, backend_command, backend_cli_available, backend_ffi_available, backend_available, backend_kind, backend_version, backend_last_error, backend_report, backend_dlopen_checked, backend_clear_cache, flint_available, pari_available, z3_available)
use std.core
use std.os
use std.os.path as ospath
use std.os.ffi (RTLD_NOW, RTLD_GLOBAL, dlopen_checked, dlopen_any, dlclose)
use std.core.str as str
use std.core.common as common

mut _backend_cache = dict(16)
mut _backend_errors = dict(16)

fn _backend_norm(any: name): str {
   def n = str.lower(str.strip(to_str(name)))
   case n {
      "pari/gp", "parigp", "gp" -> "pari"
      _ -> n
   }
}

fn _backend_removed_lattice_cli(any: name): bool {
   def n = _backend_norm(name)
   n == "fplll" || n == "fplll-cli" || n == "flatter"
}

fn _backend_env_key(any: name, str: suffix): str {
   "Return an environment variable key for a backend."
   "NY_" + str.upper(_backend_norm(name)) + "_" + suffix
}

fn backend_clear_cache(): any {
   "Clear cached optional math backend discovery results."
   _backend_cache = dict(16)
   _backend_errors = dict(16)
}

fn backend_mode(any: name=""): str {
   "Return backend mode for `name`: auto, pure, native, ffi, cli, or off."
   def specific = common.env_lower(_backend_env_key(name, "BACKEND"))
   if(specific.len > 0){ return specific }
   def global = common.env_lower("NY_MATH_BACKEND")
   global.len > 0 ? global : "auto"
}

fn native_enabled(any: name): bool {
   "Return whether optional native/CLI backends are allowed for `name`."
   if(_backend_removed_lattice_cli(name)){ return false }
   case backend_mode(name){
      "0", "false", "no", "off", "pure" -> false
      _ -> true
   }
}

fn backend_command(any: name): str {
   "Return the command used by a CLI backend, honoring per-backend overrides."
   def n = _backend_norm(name)
   if(_backend_removed_lattice_cli(n)){ return "" }
   def modern = common.env_trim(_backend_env_key(n, "CMD"))
   if(modern.len > 0){ return modern }
   def env_key = case n {
      "pari" -> "GP"
      _ -> _backend_env_key(n, "CMD")
   }
   def override = common.env_trim(env_key)
   if(override.len > 0){ return override }
   case n {
      "pari" -> "gp"
      _ -> n
   }
}

fn _cmd_exists(str: cmd): bool {
   if(!is_str(cmd) || cmd.len == 0){ return false }
   if(file_exists(cmd)){ return true }
   def p = env("PATH")
   if(!is_str(p) || p.len == 0){ return false }
   mut sep = ":"
   #windows { sep = ";" }
   #endif
   def dirs = str.split(p, sep)
   mut i = 0
   while(i < dirs.len){
      def d = dirs.get(i, "")
      if(d.len > 0){
         def base = ospath.join(d, cmd)
         if(file_exists(base)){ return true }
         #windows {
            if(file_exists(base + ".exe")){ return true }
            if(file_exists(base + ".cmd")){ return true }
            if(file_exists(base + ".bat")){ return true }
         }
         #endif
      }
      i += 1
   }
   false
}

fn _backend_lib(any: name): str {
   case _backend_norm(name){
      "pari" -> "pari"
      _ -> _backend_norm(name)
   }
}

fn _backend_required_symbol(any: name): str {
   case _backend_norm(name){
      "z3" -> "Z3_mk_config"
      _ -> ""
   }
}

fn _backend_set_error(any: name, any: msg): bool {
   _backend_errors = _backend_errors.set(_backend_norm(name), to_str(msg))
   false
}

fn backend_dlopen_checked(any: name, any: lib="", any: required_symbol=""): any {
   "Open a backend library when native backends are enabled; returns a handle or 0."
   def n = _backend_norm(name)
   if(_backend_removed_lattice_cli(n)){
      _backend_set_error(n, "external lattice backend removed; use std.math.crypto.lattice")
      return 0
   }
   if(!native_enabled(n)){
      _backend_set_error(n, "disabled by " + _backend_env_key(n, "BACKEND") + " or NY_MATH_BACKEND")
      return 0
   }
   if(backend_mode(n) == "cli"){
      _backend_set_error(n, "ffi disabled by " + _backend_env_key(n, "BACKEND") + "=cli")
      return 0
   }
   mut lname = lib
   if(!is_str(lname) || lname.len == 0){ lname = _backend_lib(n) }
   if(!is_str(lname) || lname.len == 0){
      _backend_set_error(n, "no library backend")
      return 0
   }
   mut sym = required_symbol
   if(!is_str(sym)){ sym = "" }
   if(sym.len == 0){ sym = _backend_required_symbol(n) }
   mut h = 0
   if(sym.len > 0){
      h = dlopen_checked(lname, sym, RTLD_NOW() | RTLD_GLOBAL())
   } else {
      h = dlopen_any(lname, RTLD_NOW() | RTLD_GLOBAL())
   }
   if(!h){ _backend_set_error(n, "library not found: " + lname) }
   h
}

fn backend_ffi_available(any: name): bool {
   "Return true when a backend shared library can be opened."
   def n = _backend_norm(name)
   def mode = backend_mode(n)
   if(mode == "cli" || !native_enabled(n)){ return false }
   def h = backend_dlopen_checked(n)
   if(!h){ return false }
   dlclose(h)
   true
}

fn backend_cli_available(any: name): bool {
   "Return true when a backend command is available."
   def n = _backend_norm(name)
   if(_backend_removed_lattice_cli(n)){
      _backend_set_error(n, "external lattice CLI backend removed; use std.math.crypto.lattice")
      return false
   }
   def mode = backend_mode(n)
   if(mode == "ffi" || mode == "native" || !native_enabled(n)){ return false }
   def cmd = backend_command(n)
   def ok = _cmd_exists(cmd)
   if(!ok){ _backend_set_error(n, "command not found: " + cmd) }
   ok
}

fn _backend_prefer_cli(any: n): bool { false }

fn backend_kind(any: name): str {
   "Return the selected backend kind: ffi, cli, pure, or missing."
   def n = _backend_norm(name)
   if(_backend_removed_lattice_cli(n)){ return "pure" }
   def mode = backend_mode(n)
   if(!native_enabled(n)){ return "pure" }
   if(mode == "cli"){ return backend_cli_available(n) ? "cli" : "missing" }
   if(mode == "ffi" || mode == "native"){ return backend_ffi_available(n) ? "ffi" : "missing" }
   if(_backend_prefer_cli(n)){
      if(backend_cli_available(n)){ return "cli" }
      if(backend_ffi_available(n)){ return "ffi" }
      return "missing"
   }
   if(backend_ffi_available(n)){ return "ffi" }
   if(backend_cli_available(n)){ return "cli" }
   "missing"
}

fn backend_available(any: name): bool {
   "Return true when a backend is usable under the current mode."
   def k = backend_kind(name)
   k == "ffi" || k == "cli"
}

fn backend_version(any: name): str {
   "Return a compact backend version/status string when available."
   def k = backend_kind(name)
   if(k == "missing" || k == "pure"){ return "" }
   k
}

fn backend_last_error(any: name): str {
   "Return the last discovery error for `name`, or an empty string."
   _backend_errors.get(_backend_norm(name), "")
}

fn backend_report(): dict {
   "Return a dict describing optional math backend availability."
   mut out = dict(8)
   def names = ["flint", "pari", "z3"]
   mut i = 0
   while(i < names.len){
      def n = names[i]
      mut rec = dict(6)
      rec["mode"] = backend_mode(n)
      rec["kind"] = backend_kind(n)
      rec["available"] = backend_available(n)
      rec["command"] = backend_command(n)
      rec["version"] = backend_version(n)
      rec["error"] = backend_last_error(n)
      out[n] = rec
      i += 1
   }
   out
}

fn flint_available(): bool {
   "Return true when FLINT is available."
   backend_available("flint")
}

fn pari_available(): bool {
   "Return true when PARI/GP is available."
   backend_available("pari")
}

fn z3_available(): bool {
   "Return true when Z3 is available."
   backend_available("z3")
}
