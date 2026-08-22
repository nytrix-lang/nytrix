;; Keywords: symbolic reverse unicorn z3 smt
;; Symbolic execution tools for reversing. Z3 handles constraints, and Unicorn
;; is loaded only when present so scripts can import this module everywhere.
module std.os.rev.symbolic(
   sleep, msleep,
   unicorn_available, unicorn_version, unicorn_arch_supported, unicorn_constants,
   unicorn_open, unicorn_close, unicorn_error, unicorn_session, unicorn_plan,
   unicorn_mem_map, unicorn_mem_write, unicorn_mem_read, unicorn_reg_id,
   unicorn_reg_write_u64, unicorn_reg_read_u64, unicorn_emu_start,
   unicorn_run, unicorn_run_x86_64,
   z3_available, symbolic_available, backend_status, arch_info,
   project, procedure, procedure_library, project_proc, project_hook, project_hooks, project_load, project_loads, blob_project, shellcode_project, blank_state, entry_state, call_state,
   simgr, simgr_step, simgr_lifted_step, simgr_stashes, simgr_stash, simgr_summary,
   state_solution, state_solutions, simgr_solution, simgr_solutions, simgr_solutions_all,
   simgr_move, simgr_drop, simgr_prune, explore, run_until, run_lifted_until, watch_until, watch_lifted_until, find, avoid,
   find_reg, avoid_reg, find_mem, avoid_mem, find_stdout, avoid_stdout,
   find_stderr, avoid_stderr, find_output, avoid_output, state_matches,
   state_addr, state_regs, state_mem, state_constraints, state_symbolics, state_feasible,
   state_history, state_trace, state_path, state_path_text, simgr_traces,
   state_set_addr, state_set_reg, state_get_reg, mem_write, mem_write_byte, mem_write_bytes, mem_read, mem_read_bytes,
   state_stdout, state_stderr, state_output, state_append_stdout, state_append_stderr,
   state_stdin, state_argv, state_env, state_set_stdin, state_symbolic_stdin,
   state_constrain_stdin_eq, state_constrain_stdin_range, state_eval_stdin_ascii,
   state_set_argv, state_symbolic_argv, state_constrain_argv_eq, state_constrain_argv_range, state_eval_argv_ascii,
   state_set_env, state_set_fs, state_fs, state_set_net, state_net, state_set_time, state_set_random, state_maps, state_process,
   state_solver, state_add, state_symbolic_reg, state_constrain_reg_eq, state_constrain_reg_range, state_eval_reg_u64, state_eval_reg_hex,
   state_constrain_symbolic_eq, state_constrain_symbolic_range,
   state_symbolic_bytes, state_symbolic_mem, state_constrain_mem_eq, state_constrain_mem_range, state_eval_mem_byte_u64,
   state_eval_mem_bytes, state_eval_mem_ascii,
   state_eval_ascii, state_clone, state_step, state_successors, state_lifted_successors,
   state_fork, state_summary, state_snapshot, state_diff, state_diff_matches,
   watch_addr, watch_reg, watch_mem, watch_stdout, watch_stderr, watch_output,
   solver, solver_free, solver_add, solver_check, solver_sat, solver_model,
   solver_block_hex, enumerate_hex,
   bvs, bvv, bytes_symbolic, byte_constraints_ascii, byte_constraints_eq,
   byte_constraints_xor_eq, eval_u64, eval_hex, eval_bytes, eval_ascii,
   solve_bytes_xor_eq, solve_ascii_sum8,
   UC_ARCH_X86, UC_ARCH_ARM, UC_ARCH_ARM64, UC_ARCH_RISCV, UC_MODE_16, UC_MODE_32, UC_MODE_64,
   UC_MODE_ARM, UC_MODE_THUMB, UC_MODE_RISCV32, UC_MODE_RISCV64, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC,
   UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
   UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RBP, UC_X86_REG_RSP,
   UC_X86_REG_RIP, UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
   UC_X86_REG_EDX, UC_X86_REG_EIP,
   UC_RISCV_REG_PC, UC_RISCV_REG_A0, UC_RISCV_REG_A1, UC_RISCV_REG_A2,
   UC_RISCV_REG_A3, UC_RISCV_REG_A4, UC_RISCV_REG_A5, UC_RISCV_REG_A6,
   UC_RISCV_REG_A7,
   SAT, UNSAT, UNKNOWN,
)

use std.core
use std.core.str as str
use std.os.ffi as ffi
use std.os.time as ostime
use std.math.smt as smt
use std.math.parse.data.zlib as zlib

fn to_hex(int v, int width=0) str {
   str.to_hex(v, width)
}

def SAT = 1
def UNSAT = -1
def UNKNOWN = 0

fn sleep(any seconds) any {
   "Pause the current thread for `seconds`; useful for paced emulation/reversing scripts."
   ostime.sleep(int(seconds))
}

fn msleep(any ms) any {
   "Pause the current thread for `ms` milliseconds."
   ostime.msleep(int(ms))
}

def UC_ARCH_ARM = 1
def UC_ARCH_ARM64 = 2
def UC_ARCH_X86 = 4
def UC_ARCH_RISCV = 8
def UC_MODE_16 = 2
def UC_MODE_32 = 4
def UC_MODE_64 = 8
def UC_MODE_ARM = 0
def UC_MODE_THUMB = 16
def UC_MODE_RISCV32 = 4
def UC_MODE_RISCV64 = 8
def UC_PROT_READ = 1
def UC_PROT_WRITE = 2
def UC_PROT_EXEC = 4
def UC_X86_REG_EAX = 19
def UC_X86_REG_EBX = 21
def UC_X86_REG_ECX = 22
def UC_X86_REG_EDX = 24
def UC_X86_REG_EIP = 26
def UC_X86_REG_RAX = 35
def UC_X86_REG_RBP = 36
def UC_X86_REG_RBX = 37
def UC_X86_REG_RCX = 38
def UC_X86_REG_RDI = 39
def UC_X86_REG_RDX = 40
def UC_X86_REG_RIP = 41
def UC_X86_REG_RSI = 43
def UC_X86_REG_RSP = 44
def UC_RISCV_REG_A0 = 11
def UC_RISCV_REG_A1 = 12
def UC_RISCV_REG_A2 = 13
def UC_RISCV_REG_A3 = 14
def UC_RISCV_REG_A4 = 15
def UC_RISCV_REG_A5 = 16
def UC_RISCV_REG_A6 = 17
def UC_RISCV_REG_A7 = 18
def UC_RISCV_REG_PC = 190
mut _uc = 0
mut _p_uc_version = 0
mut _p_uc_arch_supported = 0
mut _p_uc_open = 0
mut _p_uc_close = 0
mut _p_uc_strerror = 0
mut _p_uc_mem_map = 0
mut _p_uc_mem_write = 0
mut _p_uc_mem_read = 0
mut _p_uc_reg_write = 0
mut _p_uc_reg_read = 0
mut _p_uc_emu_start = 0

fn _load_unicorn() bool {
   if _uc { return true }
   def flags = ffi.RTLD_NOW() | ffi.RTLD_GLOBAL()
   mut h = ffi.dlopen_checked("unicorn", "uc_version", flags)
   if !h { h = ffi.dlopen_checked("libunicorn.so.2", "uc_version", flags) }
   if !h { h = ffi.dlopen_checked("libunicorn.so.1", "uc_version", flags) }
   if !h { h = ffi.dlopen_checked("unicorn.dll", "uc_version", flags) }
   if !h { h = ffi.dlopen_checked("libunicorn.dylib", "uc_version", flags) }
   if !h { return false }
   _uc = h
   _p_uc_version = ffi.dlsym(h, "uc_version")
   _p_uc_arch_supported = ffi.dlsym(h, "uc_arch_supported")
   _p_uc_open = ffi.dlsym(h, "uc_open")
   _p_uc_close = ffi.dlsym(h, "uc_close")
   _p_uc_strerror = ffi.dlsym(h, "uc_strerror")
   _p_uc_mem_map = ffi.dlsym(h, "uc_mem_map")
   _p_uc_mem_write = ffi.dlsym(h, "uc_mem_write")
   _p_uc_mem_read = ffi.dlsym(h, "uc_mem_read")
   _p_uc_reg_write = ffi.dlsym(h, "uc_reg_write")
   _p_uc_reg_read = ffi.dlsym(h, "uc_reg_read")
   _p_uc_emu_start = ffi.dlsym(h, "uc_emu_start")
   _p_uc_version != 0
}

fn _arch_id(any arch) int {
   def a = str.lower(str.strip(to_str(arch)))
   match a {
      "x86", "i386", "x86_32" -> UC_ARCH_X86
      "x86_64", "amd64" -> UC_ARCH_X86
      "arm" -> UC_ARCH_ARM
      "arm64", "aarch64" -> UC_ARCH_ARM64
      "riscv", "riscv64", "rv64", "riscv32", "rv32" -> UC_ARCH_RISCV
      _ -> int(arch)
   }
}

fn _mode_id(any mode) int {
   if is_int(mode) { return int(mode) }
   mut out = 0
   def parts = str.split(str.lower(str.strip(to_str(mode))), "|")
   mut i = 0
   while i < parts.len {
      def p = str.strip(parts[i])
      match p {
         "16", "x16", "mode16" -> { out = out | UC_MODE_16 }
         "32", "x32", "mode32" -> { out = out | UC_MODE_32 }
         "64", "x64", "mode64" -> { out = out | UC_MODE_64 }
         "arm" -> { out = out | UC_MODE_ARM }
         "thumb" -> { out = out | UC_MODE_THUMB }
         "riscv32", "rv32" -> { out = out | UC_MODE_RISCV32 }
         "riscv64", "rv64" -> { out = out | UC_MODE_RISCV64 }
         _ -> { }
      }
      i += 1
   }
   out
}

fn _arch_name(any arch) str {
   def id = _arch_id(arch)
   if id == UC_ARCH_X86 { return "x86" }
   if id == UC_ARCH_ARM { return "arm" }
   if id == UC_ARCH_ARM64 { return "arm64" }
   if id == UC_ARCH_RISCV { return "riscv" }
   "unknown"
}

fn _default_bits(any arch, any mode) int {
   def m = _mode_id(mode)
   if (m & UC_MODE_64) != 0 { return 64 }
   if (m & UC_MODE_32) != 0 { return 32 }
   if _arch_id(arch) == UC_ARCH_ARM64 { return 64 }
   32
}

fn unicorn_available() bool {
   "Returns true when the Unicorn shared library can be loaded."
   _load_unicorn()
}

fn unicorn_version() list {
   "Returns [major, minor, combined] from Unicorn, or [] when unavailable."
   if !_load_unicorn() || !_p_uc_version { return [] }
   def maj = ffi.malloc(4)
   def minp = ffi.malloc(4)
   if !maj || !minp {
      if maj { ffi.free(maj) }
      if minp { ffi.free(minp) }
      return []
   }
   store32(maj, 0, 0)
   store32(minp, 0, 0)
   def combined = ffi.call2(_p_uc_version, maj, minp)
   def out = [load32(maj, 0), load32(minp, 0), combined]
   ffi.free(maj)
   ffi.free(minp)
   out
}

fn unicorn_arch_supported(any arch) bool {
   "Returns true if Unicorn reports support for `arch`."
   if !_load_unicorn() || !_p_uc_arch_supported { return false }
   ffi.call1(_p_uc_arch_supported, _arch_id(arch)) != 0
}

fn unicorn_constants() dict {
   "Returns useful Unicorn constants for API callers."
   {
      "arch": {"x86": UC_ARCH_X86, "arm": UC_ARCH_ARM, "arm64": UC_ARCH_ARM64, "riscv": UC_ARCH_RISCV},
      "mode": {"16": UC_MODE_16, "32": UC_MODE_32, "64": UC_MODE_64, "arm": UC_MODE_ARM, "thumb": UC_MODE_THUMB, "riscv32": UC_MODE_RISCV32, "riscv64": UC_MODE_RISCV64},
      "prot": {"read": UC_PROT_READ, "write": UC_PROT_WRITE, "exec": UC_PROT_EXEC, "all": UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC},
   }
}

fn unicorn_error(any code) str {
   "Returns Unicorn error text for an error code when available."
   if !_load_unicorn() || !_p_uc_strerror { return "unicorn error " + to_str(code) }
   def p = ffi.call1_ptr(_p_uc_strerror, int(code))
   p ? str.cstr_to_str(__untag(p)) : ("unicorn error " + to_str(code))
}

fn unicorn_open(any arch="x86_64", any mode="64") dict {
   "Open a Unicorn engine when libunicorn is available.
   Returns {ok, engine, code, arch, mode} ; callers should pass the result to
   unicorn_close when ok is true. This is intentionally low-level and keeps the
   native handle visible for advanced FFI work."
   if !_load_unicorn() || !_p_uc_open { return {"ok": false, "engine": 0, "code": -1, "reason": "unicorn_unavailable"} }
   def outp = ffi.malloc(8)
   if !outp { return {"ok": false, "engine": 0, "code": -2, "reason": "alloc_failed"} }
   store64_h(outp, 0, 0)
   def a = _arch_id(arch)
   mut m = _mode_id(mode)
   if a == UC_ARCH_ARM64 && m == UC_MODE_64 { m = UC_MODE_ARM }
   def code = ffi.ffi_call(_p_uc_open, [a, m, outp])
   def engine = load64_h(outp, 0)
   ffi.free(outp)
   return {"ok": (code == 0 && engine != 0), "engine": engine, "code": code, "arch": _arch_name(arch), "arch_id": a, "mode": m}
}

fn unicorn_close(any engine) bool {
   "Close a Unicorn engine handle."
   if !_load_unicorn() || !_p_uc_close || !engine { return false }
   ffi.ffi_call(_p_uc_close, [engine]) == 0
}

fn _bytestr(any data) str {
   if is_str(data) || is_bytes(data) { return data }
   if is_list(data) {
      mut out = str.Builder(max(1, data.len))
      mut i = 0
      while i < data.len {
         out = str.builder_append_byte(out, int(data[i]) & 255)
         i += 1
      }
      def text = str.builder_to_str(out)
      str.builder_free(out)
      return text
   }
   ""
}

fn _data_len(any data) int {
   if is_str(data) || is_bytes(data) || is_list(data) { return data.len }
   0
}

fn _copy_bytes(any data) any {
   if is_list(data) {
      def p = ffi.malloc(max(1, data.len))
      if !p { return [0, 0] }
      mut i = 0
      while i < data.len {
         store8(p, int(data[i]) & 255, i)
         i += 1
      }
      return [p, data.len]
   }
   def s = _bytestr(data)
   def p = ffi.malloc(max(1, s.len))
   if !p { return [0, 0] }
   mut i = 0
   while i < s.len {
      store8(p, load8(s, i), i)
      i += 1
   }
   [p, s.len]
}

fn _ptr_to_bytes(any p, int n) str {
   mut out = str.Builder(max(1, n))
   mut i = 0
   while i < n {
      out = str.builder_append_byte(out, load8(p, i) & 255)
      i += 1
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn _ptr_to_list(any p, int n) list {
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(load8(p, i) & 255)
      i += 1
   }
   out
}

fn unicorn_mem_map(any engine, int addr, int size, int perms=UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC) dict {
   "Map memory in a Unicorn engine."
   if !_load_unicorn() || !_p_uc_mem_map || !engine { return {"ok": false, "code": -1, "reason": "unicorn_unavailable"} }
   def code = ffi.ffi_call(_p_uc_mem_map, [engine, addr, size, perms])
   {"ok": code == 0, "code": code, "reason": code == 0 ? "ok" : unicorn_error(code), "addr": addr, "size": size, "perms": perms}
}

fn unicorn_mem_write(any engine, int addr, any data) dict {
   "Write concrete bytes into Unicorn memory."
   if !_load_unicorn() || !_p_uc_mem_write || !engine { return {"ok": false, "code": -1, "reason": "unicorn_unavailable"} }
   def cp = _copy_bytes(data)
   def p = cp[0]
   def n = cp[1]
   if !p { return {"ok": false, "code": -2, "reason": "alloc_failed"} }
   def code = ffi.ffi_call(_p_uc_mem_write, [engine, addr, p, n])
   ffi.free(p)
   return {"ok": code == 0, "code": code, "reason": code == 0 ? "ok" : unicorn_error(code), "addr": addr, "size": n}
}

fn unicorn_mem_read(any engine, int addr, int size) dict {
   "Read concrete bytes from Unicorn memory."
   if !_load_unicorn() || !_p_uc_mem_read || !engine { return {"ok": false, "code": -1, "reason": "unicorn_unavailable", "data": ""} }
   def p = ffi.malloc(max(1, size))
   if !p { return {"ok": false, "code": -2, "reason": "alloc_failed", "data": ""} }
   def code = ffi.ffi_call(_p_uc_mem_read, [engine, addr, p, size])
   def data = code == 0 ? _ptr_to_list(p, size) : []
   ffi.free(p)
   return {"ok": code == 0, "code": code, "reason": code == 0 ? "ok" : unicorn_error(code), "addr": addr, "size": size, "data": data}
}

fn unicorn_reg_id(any reg) int {
   "Resolve common x86/x86_64 register names to Unicorn constants."
   if is_int(reg) { return int(reg) }
   match str.lower(str.strip(to_str(reg))) {
      "rax" -> UC_X86_REG_RAX
      "rbx" -> UC_X86_REG_RBX
      "rcx" -> UC_X86_REG_RCX
      "rdx" -> UC_X86_REG_RDX
      "rdi" -> UC_X86_REG_RDI
      "rsi" -> UC_X86_REG_RSI
      "rbp" -> UC_X86_REG_RBP
      "rsp" -> UC_X86_REG_RSP
      "rip", "pc" -> UC_X86_REG_RIP
      "eax" -> UC_X86_REG_EAX
      "ebx" -> UC_X86_REG_EBX
      "ecx" -> UC_X86_REG_ECX
      "edx" -> UC_X86_REG_EDX
      "eip" -> UC_X86_REG_EIP
      "a0", "x10" -> UC_RISCV_REG_A0
      "a1", "x11" -> UC_RISCV_REG_A1
      "a2", "x12" -> UC_RISCV_REG_A2
      "a3", "x13" -> UC_RISCV_REG_A3
      "a4", "x14" -> UC_RISCV_REG_A4
      "a5", "x15" -> UC_RISCV_REG_A5
      "a6", "x16" -> UC_RISCV_REG_A6
      "a7", "x17" -> UC_RISCV_REG_A7
      "riscv_pc" -> UC_RISCV_REG_PC
      _ -> 0
   }
}

fn unicorn_reg_write_u64(any engine, any reg, any value) dict {
   "Write a u64 register value into Unicorn."
   if !_load_unicorn() || !_p_uc_reg_write || !engine { return {"ok": false, "code": -1, "reason": "unicorn_unavailable"} }
   def p = ffi.malloc(8)
   if !p { return {"ok": false, "code": -2, "reason": "alloc_failed"} }
   store64_h(p, value, 0)
   def rid = unicorn_reg_id(reg)
   def code = ffi.ffi_call(_p_uc_reg_write, [engine, rid, p])
   ffi.free(p)
   return {"ok": code == 0, "code": code, "reason": code == 0 ? "ok" : unicorn_error(code), "reg": rid, "value": value}
}

fn unicorn_reg_read_u64(any engine, any reg) dict {
   "Read a u64 register value from Unicorn."
   if !_load_unicorn() || !_p_uc_reg_read || !engine { return {"ok": false, "code": -1, "reason": "unicorn_unavailable", "value": 0} }
   def p = ffi.malloc(8)
   if !p { return {"ok": false, "code": -2, "reason": "alloc_failed", "value": 0} }
   store64_h(p, 0, 0)
   def rid = unicorn_reg_id(reg)
   def code = ffi.ffi_call(_p_uc_reg_read, [engine, rid, p])
   def value = load64_h(p, 0)
   ffi.free(p)
   return {"ok": code == 0, "code": code, "reason": code == 0 ? "ok" : unicorn_error(code), "reg": rid, "value": value}
}

fn unicorn_emu_start(any engine, int begin, int until=0, int timeout=0, int count=0) dict {
   "Run Unicorn emulation."
   if !_load_unicorn() || !_p_uc_emu_start || !engine { return {"ok": false, "code": -1, "reason": "unicorn_unavailable"} }
   def code = ffi.ffi_call(_p_uc_emu_start, [engine, begin, until, timeout, count])
   {"ok": code == 0, "code": code, "reason": code == 0 ? "ok" : unicorn_error(code), "begin": begin, "until": until, "count": count}
}

fn _page_base(int addr) int { addr & -4096 }

fn _page_size(int size) int { ((max(1, size) + 4095) / 4096) * 4096 }

fn _default_unicorn_mode(any arch) str {
   def a = str.lower(str.strip(to_str(arch)))
   if a == "x86_64" || a == "amd64" { return "64" }
   if a == "arm64" || a == "aarch64" { return "arm" }
   if a == "arm" { return "arm" }
   if a == "riscv32" || a == "rv32" { return "riscv32" }
   if a == "riscv" || a == "riscv64" || a == "rv64" { return "riscv64" }
   "32"
}

fn _default_read_regs(any arch) list {
   def a = str.lower(str.strip(to_str(arch)))
   if a == "x86_64" || a == "amd64" { return ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip"] }
   if a == "x86" || a == "i386" || a == "x86_32" { return ["eax", "ebx", "ecx", "edx", "eip"] }
   if a == "riscv" || a == "riscv64" || a == "rv64" || a == "riscv32" || a == "rv32" { return ["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"] }
   []
}

fn unicorn_run(any code, any regs=dict(), any opts=dict()) dict {
   "Run raw machine code with Unicorn for any supported architecture.
   Named registers are resolved when known ; numeric Unicorn register IDs work
   for every ISA, which keeps this wrapper useful for ARM/AArch64/RISC-V style
   reversing without baking every backend constant into Nytrix."
   def arch = opts.get("arch", "x86_64")
   def mode = opts.get("mode", _default_unicorn_mode(arch))
   def base = int(opts.get("base", 0x400000))
   def count = int(opts.get("count", 0))
   def opened = unicorn_open(arch, mode)
   if !opened.get("ok", false) { return {"ok": false, "reason": opened.get("reason", "open_failed"), "open": opened} }
   def uc = opened.get("engine", 0)
   defer { unicorn_close(uc) }
   def data_len = _data_len(code)
   def map = unicorn_mem_map(uc, _page_base(base), _page_size(data_len), UC_PROT_READ | UC_PROT_WRITE | UC_PROT_EXEC)
   if !map.get("ok", false) { return {"ok": false, "reason": "map_failed", "map": map} }
   def wr = unicorn_mem_write(uc, base, code)
   if !wr.get("ok", false) { return {"ok": false, "reason": "write_failed", "write": wr} }
   def rkeys = regs.keys()
   mut ri = 0
   while ri < rkeys.len {
      def k = rkeys[ri]
      def rr = unicorn_reg_write_u64(uc, k, regs.get(k, 0))
      if !rr.get("ok", false) { return {"ok": false, "reason": "reg_write_failed", "reg": k, "detail": rr} }
      ri += 1
   }
   def run = unicorn_emu_start(uc, base, int(opts.get("until", base + data_len)), int(opts.get("timeout", 0)), count)
   mut out = dict(16)
   def reg_names = opts.get("read_regs", _default_read_regs(arch))
   mut oi = 0
   while oi < reg_names.len {
      def name = reg_names[oi]
      def rv = unicorn_reg_read_u64(uc, name)
      if rv.get("ok", false) { out = out.set(to_str(name), rv.get("value", 0)) }
      oi += 1
   }
   {"ok": run.get("ok", false), "reason": run.get("reason", ""), "arch": _arch_name(arch), "mode": _mode_id(mode),
      "base": base, "size": data_len, "run": run, "regs": out}
}

fn unicorn_run_x86_64(any code, any regs=dict(), int base=0x400000, int count=0) dict {
   "Run raw x86_64 machine code with Unicorn when available. Returns final
   common register values. This is the concrete-execution companion to the
   symbolic solver helpers."
   unicorn_run(code, regs, {"arch": "x86_64", "mode": "64", "base": base, "count": count,
         "read_regs": ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip"]})
}

fn unicorn_session(any arch="x86_64", any mode="64", any opts=dict()) dict {
   "Create an angr-style Unicorn session record. It includes a native engine
   handle when available, otherwise a reasoned stub that can still drive docs
   and planning code."
   def opened = unicorn_open(arch, mode)
   {"kind": "unicorn_session", "available": opened.get("ok", false), "engine": opened.get("engine", 0),
      "arch": _arch_name(arch), "mode": _mode_id(mode), "bits": _default_bits(arch, mode), "options": opts,
      "reason": opened.get("ok", false) ? "ready" : opened.get("reason", "open_failed"), "native": opened}
}

fn unicorn_plan(any arch="x86_64", any mode="64", int base=0x400000, any code="", int count=0) dict {
   "Returns an execution plan for Unicorn-backed emulation without requiring
   Unicorn at import time."
   {"engine": "unicorn", "available": unicorn_available(), "arch": _arch_name(arch), "mode": _mode_id(mode),
      "bits": _default_bits(arch, mode), "base": base, "code_len": (is_str(code) || is_bytes(code)) ? code.len : 0,
      "count": count, "implemented": unicorn_available(), "missing": unicorn_available() ? [] : ["libunicorn"]}
}

fn z3_available() bool {
   "Returns true when native Z3 is available."
   smt.z3_available()
}

fn symbolic_available() bool {
   "Returns true when the solving side is available. Unicorn is optional."
   z3_available()
}

fn backend_status() dict {
   "Returns symbolic backend availability and version metadata."
   {"z3": z3_available(), "z3_version": smt.z3_version_str(), "unicorn": unicorn_available(),
      "unicorn_version": unicorn_version(), "ready": symbolic_available()}
}

fn arch_info(any arch="x86_64", any mode="64") dict {
   "Normalize architecture and word-size metadata."
   {"arch": _arch_name(arch), "arch_id": _arch_id(arch), "mode": _mode_id(mode), "bits": _default_bits(arch, mode),
      "unicorn_supported": unicorn_arch_supported(arch)}
}

fn project(any target="", any opts=dict()) dict {
   "Create a lightweight angr-style project record for binary analysis code."
   def arch = opts.get("arch", "x86_64")
   def mode = opts.get("mode", "64")
   {"kind": "project", "target": target, "arch": arch_info(arch, mode), "options": opts,
      "entry": opts.get("entry", 0), "base": opts.get("base", 0), "backend": backend_status()}
}

fn project_hooks(dict proj) dict {
   "Return project hook table keyed by address string."
   proj.get("hooks", dict())
}

fn _proc_name(any name) str {
   mut n = str.lower(str.strip(to_str(name)))
   def at = str.find(n, "@")
   if at >= 0 { n = slice(n, 0, at, 1) }
   if str.startswith(n, "__isoc99_") { n = slice(n, 9, n.len, 1) }
   if str.startswith(n, "__isoc23_") { n = slice(n, 9, n.len, 1) }
   if str.startswith(n, "_") { n = slice(n, 1, n.len, 1) }
   n
}

fn procedure_library() list {
   "Return built-in SimProcedure summary names understood by hooks."
   ["puts", "printf", "write", "fwrite", "putchar", "getchar", "read",
      "open", "openat", "close", "lseek", "mmap", "mprotect", "munmap", "brk",
      "access", "faccessat", "stat", "lstat", "fstat", "newfstatat", "readlink", "readlinkat", "uname",
      "socket", "connect", "recv", "send", "recvfrom", "sendto",
      "dup", "dup2", "dup3", "pipe", "pipe2", "fcntl", "ioctl", "readv", "writev",
      "getenv", "time", "gettimeofday", "clock_gettime", "getpid", "getppid",
      "gettid", "getuid", "geteuid", "getgid", "getegid", "getrandom", "rand", "srand",
      "getauxval", "dlopen", "dlsym", "dlclose", "dlerror",
      "ptrace", "prctl", "arch_prctl", "sleep", "usleep", "nanosleep",
      "kill", "raise", "tgkill", "fork", "vfork", "clone", "execve", "wait", "waitpid", "wait4", "system",
      "scanf", "__isoc99_scanf", "__isoc23_scanf", "fscanf", "sscanf", "strlen",
      "strcmp", "strncmp", "strcasecmp", "strncasecmp", "memcmp", "bcmp",
      "memchr", "strchr", "strrchr", "strstr",
      "memcpy", "memmove", "memset", "memfrob", "strcpy", "strncpy",
      "strdup", "fgets", "gets", "atoi", "atol", "atoll", "strtol", "strtoll",
      "uncompress",
      "malloc", "calloc", "realloc", "free", "exit", "_exit", "abort"]
}

fn procedure(any name, any opts=dict()) dict {
   "Create a data-shaped SimProcedure hook summary.
   Known procedures model common libc imports. Unknown names still become hooks
   with a stable `proc` name so callers can inspect and replace them later."
   def nm = to_str(name)
   def proc = _proc_name(nm)
   def base = {"name": nm, "proc": proc, "size": int(is_dict(opts) ? opts.get("size", 1) : 1)}
   is_dict(opts) ? base.merge(opts).set("name", opts.get("name", nm)).set("proc", opts.get("proc", proc)) : base
}

fn project_proc(dict proj, int addr, any name, any opts=dict()) dict {
   "Return a project with a named SimProcedure hook at `addr`."
   project_hook(proj, addr, procedure(name, opts).set("addr", addr))
}

fn project_hook(dict proj, int addr, any spec) dict {
   "Return a project with a SimProcedure-like hook at `addr`.
   Supported hook fields name, proc, ret, size, regs, mem, stdout, stderr,
   deadend. Hooks are intentionally data-shaped so reverse-engineering scripts
   can define cheap procedure summaries without native callbacks."
   mut h = is_dict(spec) ? spec : {"name": to_str(spec)}
   if !h.contains("name") { h = h.set("name", "hook_" + to_hex(addr, 0)) }
   if !h.contains("addr") { h = h.set("addr", addr) }
   dict_clone(proj).set("hooks", proj.get("hooks", dict()).set(to_str(addr), h))
}

fn project_loads(dict proj) list {
   "Return mapped load ranges for a project."
   if proj.contains("loads") { return proj.get("loads", []) }
   proj.get("options", dict()).get("loads", [])
}

fn project_load(dict proj, int addr, any data, any perms="r-x", int offset=0) dict {
   "Return a project with one mapped memory load.
   Loads are {addr, size, data, perms, offset} records and are intentionally
   plain data so ELF loaders, patchers, and fuzz harnesses can share them."
   def load = {"addr": addr, "size": _data_len(data), "data": data, "perms": to_str(perms), "offset": offset}
   dict_clone(proj).set("loads", project_loads(proj).append(load))
}

fn blob_project(any bytes, any opts=dict()) dict {
   "Create a project for raw bytes/shellcode."
   def base = int(opts.get("base", 0x400000))
   project("<blob>", opts.merge({"base": base, "entry": opts.get("entry", base), "blob": bytes, "blob_len": _data_len(bytes),
            "loads": opts.get("loads", [{"addr": base, "size": _data_len(bytes), "data": bytes, "perms": "r-x", "offset": 0}])}))
}

fn shellcode_project(any code, any arch="x86_64", any base=0x400000) dict {
   "Create a project for raw shellcode bytes."
   blob_project(code, {"arch": arch, "mode": _default_unicorn_mode(arch), "base": base, "entry": base})
}

fn _abi_profile(any ai) dict {
   def a = is_dict(ai) ? ai.get("arch", to_str(ai)) : to_str(ai)
   if a == "arm64" { return {"abi": "aapcs64", "args": ["x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"], "argc": "x0", "argv": "x1", "pc": "pc", "returns": ["x0"]} }
   if a == "arm" { return {"abi": "aapcs32", "args": ["r0", "r1", "r2", "r3"], "argc": "r0", "argv": "r1", "pc": "pc", "returns": ["r0"]} }
   if a == "riscv" { return {"abi": "riscv_elf", "args": ["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"], "argc": "a0", "argv": "a1", "pc": "riscv_pc", "returns": ["a0"]} }
   if a == "x86" && is_dict(ai) && int(ai.get("bits", 64)) == 32 { return {"abi": "cdecl_i386", "args": [], "argc": "", "argv": "", "pc": "eip", "returns": ["eax"]} }
   {"abi": "sysv_x86_64", "args": ["rdi", "rsi", "rdx", "rcx", "r8", "r9"], "argc": "rdi", "argv": "rsi", "pc": "rip", "returns": ["rax"]}
}

fn _new_constraints() list { [] }

fn blank_state(any proj=0, int addr=0, any opts=dict()) dict {
   "Create a blank symbolic state with registers, memory, constraints, and solver session."
   def ai = is_dict(proj) ? proj.get("arch", arch_info(opts.get("arch", "x86_64"), opts.get("mode", "64"))) : arch_info(opts.get("arch", "x86_64"), opts.get("mode", "64"))
   {"kind": "state", "project": proj, "addr": addr, "arch": ai, "regs": dict(16), "mem": dict(32),
      "constraints": _new_constraints(), "symbolics": dict(16), "stdin": "", "argv": [], "env": dict(),
      "stdout": "", "stderr": "", "fs": dict(), "fd_table": dict(), "next_fd": 3,
      "maps": [], "brk": 0x91000000, "net": dict(), "net_tx": "",
      "auxv": is_dict(opts.get("auxv", dict())) ? opts.get("auxv", dict()) : dict(),
      "symbols": is_dict(opts.get("symbols", dict())) ? opts.get("symbols", dict()) : dict(),
      "dl_handles": dict(), "next_dl_handle": int(opts.get("next_dl_handle", 0x7f100000)),
      "next_sym_addr": int(opts.get("next_sym_addr", 0x7f200000)),
      "pid": int(opts.get("pid", 1337)), "tid": int(opts.get("tid", opts.get("pid", 1337))), "ppid": int(opts.get("ppid", 1)),
      "uid": int(opts.get("uid", 1000)), "gid": int(opts.get("gid", 1000)),
      "tls_fs": int(opts.get("tls_fs", 0)), "tls_gs": int(opts.get("tls_gs", 0)),
      "fork_pid": int(opts.get("fork_pid", 0)),
      "time": int(opts.get("time", 1700000000)), "time_nsec": int(opts.get("time_nsec", 123000000)),
      "random": opts.get("random", "NYTRIX_RANDOM_STREAM"), "random_pos": 0, "rand_seed": int(opts.get("rand_seed", 1)),
      "options": opts, "solver": solver()}
}

fn entry_state(any proj, any opts=dict()) dict {
   "Create a state at a project's entry address."
   mut st = _state_map_project_loads(blank_state(proj, int(is_dict(proj) ? proj.get("entry", 0) : 0), opts), proj)
   if opts.contains("stdin") { st = state_set_stdin(st, opts.get("stdin", "")) }
   if opts.contains("symbolic_stdin") {
      def spec = opts.get("symbolic_stdin", dict())
      if is_dict(spec) {
         st = state_symbolic_stdin(st, int(spec.get("n", spec.get("len", 0))), spec.get("name", "stdin"),
            int(spec.get("lo", 0)), int(spec.get("hi", 255)))
      } else {
         st = state_symbolic_stdin(st, int(spec), "stdin")
      }
   }
   if opts.contains("argv") { st = state_set_argv(st, opts.get("argv", []), int(opts.get("argv_base", 0x710000))) }
   if opts.contains("symbolic_argv") {
      st = state_symbolic_argv(st, opts.get("symbolic_argv", []), int(opts.get("argv_base", 0x720000)),
         int(opts.get("argv_lo", 0)), int(opts.get("argv_hi", 255)))
   }
   if opts.contains("env") { st = state_set_env(st, opts.get("env", dict())) }
   if opts.contains("fs") { st = state_set_fs(st, opts.get("fs", dict())) }
   if opts.contains("files") { st = state_set_fs(st, opts.get("files", dict())) }
   if opts.contains("net") { st = state_set_net(st, opts.get("net", dict())) }
   if opts.contains("network") { st = state_set_net(st, opts.get("network", dict())) }
   if opts.contains("time") { st = state_set_time(st, opts.get("time", 1700000000), opts.get("time_nsec", 123000000)) }
   if opts.contains("random") { st = state_set_random(st, opts.get("random", "")) }
   st
}

fn call_state(any proj, int addr, list args=[], any opts=dict()) dict {
   "Create a simple call state with architecture ABI argument registers populated."
   mut st = blank_state(proj, addr, opts)
   def regs = _abi_profile(st.get("arch", dict())).get("args", [])
   mut i = 0
   while i < args.len && i < regs.len {
      st = state_set_reg(st, regs[i], args[i])
      i += 1
   }
   st
}

fn state_addr(dict st) int { st.get("addr", 0) }

fn state_regs(dict st) dict { st.get("regs", dict()) }

fn state_mem(dict st) dict { st.get("mem", dict()) }

fn state_constraints(dict st) list { st.get("constraints", []) }

fn state_symbolics(dict st) dict { st.get("symbolics", dict()) }

fn state_stdin(dict st) any { st.get("stdin", "") }

fn state_argv(dict st) list { st.get("argv", []) }

fn state_env(dict st) dict { st.get("env", dict()) }

fn state_stdout(dict st) str { to_str(st.get("stdout", "")) }

fn state_stderr(dict st) str { to_str(st.get("stderr", "")) }

fn state_output(dict st) str { state_stdout(st) + state_stderr(st) }

fn state_append_stdout(dict st, any data) dict {
   "Return state with data appended to stdout."
   st.set("stdout", state_stdout(st) + to_str(data))
}

fn state_append_stderr(dict st, any data) dict {
   "Return state with data appended to stderr."
   st.set("stderr", state_stderr(st) + to_str(data))
}

fn state_set_stdin(dict st, any data) dict {
   "Return state with concrete stdin bytes recorded."
   st.set("stdin", data)
}

fn state_symbolic_stdin(dict st, int n, str name="stdin", int lo=0, int hi=255) dict {
   "Attach symbolic stdin bytes to a state."
   mut out = state_symbolic_bytes(st, name, n, lo, hi)
   out.set("stdin", out.get("symbolics", dict()).get(name, [])).set("stdin_symbolic", name)
}

fn _byte_eq_constraints(dict sol, list xs, any bytes) list {
   if !sol.get("ok", false) { return [] }
   def ctx = sol.get("ctx", 0)
   mut out = []
   mut i = 0
   while i < xs.len && i < _data_len(bytes) {
      out = out.append(smt.mk_eq(ctx, xs[i], smt.bv_u8(ctx, _item_at(bytes, i))))
      i += 1
   }
   out
}

fn state_constrain_stdin_eq(dict st, any bytes) dict {
   "Constrain symbolic stdin to concrete bytes."
   def nm = st.get("stdin_symbolic", "")
   if nm.len == 0 { return st }
   def xs = st.get("symbolics", dict()).get(nm, [])
   def sol = state_solver(st)
   def cs = _byte_eq_constraints(sol, xs, bytes)
   _assert_all(sol, cs)
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_eq_note("stdin", nm, bytes, cs.len))
}

fn state_constrain_stdin_range(dict st, int lo, int hi) dict {
   "Constrain symbolic stdin bytes to an inclusive unsigned range."
   def nm = st.get("stdin_symbolic", "")
   if nm.len == 0 { return st }
   def xs = st.get("symbolics", dict()).get(nm, [])
   def sol = state_solver(st)
   def cs = _byte_range_constraints(sol, xs, lo, hi)
   _assert_all(sol, cs)
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_range_note("stdin", nm, lo, hi, xs.len))
}

fn state_eval_stdin_ascii(dict st) any {
   "Evaluate symbolic stdin as ASCII, or return concrete stdin text."
   def nm = st.get("stdin_symbolic", "")
   if nm.len > 0 { return state_eval_ascii(st, nm) }
   def input = state_stdin(st)
   if is_str(input) || is_bytes(input) { return input }
   if is_list(input) {
      mut b = str.Builder(input.len)
      mut i = 0
      while i < input.len { b = str.builder_append_byte(b, int(input[i]) & 255) i += 1 }
      def s = str.builder_to_str(b)
      str.builder_free(b)
      return s
   }
   nil
}

fn _write_cstr(dict st, int addr, any text) dict {
   mem_write_bytes(st, addr, to_str(text) + "\x00")
}

fn _state_set_argv_regs(dict st, int argc, int argv_addr) dict {
   def abi = _abi_profile(st.get("arch", dict()))
   mut out = st
   def argc_reg = abi.get("argc", "")
   def argv_reg = abi.get("argv", "")
   if argc_reg.len > 0 { out = state_set_reg(out, argc_reg, argc) }
   if argv_reg.len > 0 { out = state_set_reg(out, argv_reg, argv_addr) }
   out
}

fn _ptr_size(dict st) int {
   def ai = st.get("arch", dict())
   is_dict(ai) && int(ai.get("bits", 64)) == 32 ? 4 : 8
}

fn _mem_write_ptr(dict st, int addr, int value) dict {
   _ptr_size(st) == 4 ? mem_write(st, addr, value & 0xffffffff) : mem_write(st, addr, value)
}

fn _mem_write_i32(dict st, int addr, any value) dict {
   mut out = st
   mut i = 0
   if _is_ast(value) {
      def ctx = state_solver(st).get("ctx", 0)
      while i < 4 {
         out = mem_write_byte(out, addr + i, smt.bv_extract(ctx, i * 8 + 7, i * 8, value))
         i += 1
      }
      return out
   }
   def v = int(value)
   while i < 4 {
      out = mem_write_byte(out, addr + i, (v >> (i * 8)) & 255)
      i += 1
   }
   out
}

fn _mem_write_i64(dict st, int addr, any value) dict {
   mut out = st
   def v = int(value)
   mut i = 0
   while i < 8 {
      out = mem_write_byte(out, addr + i, (v >> (i * 8)) & 255)
      i += 1
   }
   out
}

fn _mem_read_i64(dict st, int addr, int default=0) int {
   def direct = mem_read(st, addr, nil)
   if is_int(direct) && !_mem_has_byte(st, addr + 1) { return int(direct) }
   mut out = 0
   mut i = 0
   while i < 8 {
      def b = mem_read(st, addr + i, nil)
      if !is_int(b) { return default }
      out = out | ((int(b) & 255) << (i * 8))
      i += 1
   }
   out
}

fn _mem_read_i32(dict st, int addr, int default=0) int {
   def direct = mem_read(st, addr, nil)
   if is_int(direct) && !_mem_has_byte(st, addr + 1) { return int(direct) }
   mut out = 0
   mut i = 0
   while i < 4 {
      def b = mem_read(st, addr + i, nil)
      if !is_int(b) { return default }
      out = out | ((int(b) & 255) << (i * 8))
      i += 1
   }
   out
}

fn _mem_read_ptr(dict st, int addr, int default=0) int {
   _ptr_size(st) == 4 ? _mem_read_i32(st, addr, default) : _mem_read_i64(st, addr, default)
}

fn _abi_arg_reg(dict st, int index, str fallback="") str {
   def args = _abi_profile(st.get("arch", dict())).get("args", [])
   if index >= 0 && index < args.len { return args[index] }
   fallback
}

fn _abi_arg(dict st, int index, any default=0) any {
   def r = _abi_arg_reg(st, index, "")
   r.len > 0 ? _lift_get_reg(st, r, default) : default
}

fn _abi_return_reg(dict st) str {
   def returns = _abi_profile(st.get("arch", dict())).get("returns", ["rax"])
   returns.len > 0 ? returns[0] : "rax"
}

fn _abi_set_return(dict st, any value) dict {
   _lift_set_reg(st, _abi_return_reg(st), value)
}

fn _linux_syscall_profile(dict st) dict {
   def ai = st.get("arch", dict())
   def a = is_dict(ai) ? ai.get("arch", "x86") : to_str(ai)
   if a == "arm64" { return {"family": "aarch64", "nr": "x8", "ret": "x0", "args": ["x0", "x1", "x2", "x3", "x4", "x5"]} }
   if a == "arm" { return {"family": "arm", "nr": "r7", "ret": "r0", "args": ["r0", "r1", "r2", "r3", "r4", "r5"]} }
   if a == "riscv" { return {"family": "riscv", "nr": "a7", "ret": "a0", "args": ["a0", "a1", "a2", "a3", "a4", "a5"]} }
   {"family": "x86_64", "nr": "rax", "ret": "rax", "args": ["rdi", "rsi", "rdx", "r10", "r8", "r9"]}
}

fn _linux_sys_arg(dict st, dict prof, int index, any default=0) any {
   def args = prof.get("args", [])
   if index < 0 || index >= args.len { return default }
   _lift_get_reg(st, args[index], default)
}

fn _linux_sys_ret(dict st, dict prof, any value) dict {
   _lift_set_reg(st, prof.get("ret", "rax"), value)
}

fn state_set_argv(dict st, list args, int base=0x710000) dict {
   "Return state with argv strings mapped and argc/argv registers set for the state's ABI."
   mut out = st
   mut records = []
   def ptr_base = base
   mut str_base = base + 0x1000
   def psz = _ptr_size(st)
   mut i = 0
   while i < args.len {
      def s = to_str(args[i])
      out = _write_cstr(out, str_base, s)
      out = _mem_write_ptr(out, ptr_base + i * psz, str_base)
      records = records.append({"index": i, "addr": str_base, "value": s, "ptr_addr": ptr_base + i * psz})
      str_base += s.len + 1
      i += 1
   }
   out = _mem_write_ptr(out, ptr_base + args.len * psz, 0)
   _state_set_argv_regs(out.set("argv", records).set("argc", args.len).set("argv_addr", ptr_base), args.len, ptr_base)
}

fn _argv_spec_len(any spec) int {
   if is_int(spec) { return int(spec) }
   if is_dict(spec) { return int(spec.get("n", spec.get("len", 0))) }
   _data_len(spec)
}

fn _argv_spec_name(any spec, int index) str {
   if is_dict(spec) { return spec.get("name", "argv" + to_str(index)) }
   "argv" + to_str(index)
}

fn _argv_spec_lo(any spec, int fallback) int {
   is_dict(spec) ? int(spec.get("lo", fallback)) : fallback
}

fn _argv_spec_hi(any spec, int fallback) int {
   is_dict(spec) ? int(spec.get("hi", fallback)) : fallback
}

fn state_symbolic_argv(dict st, list specs, int base=0x720000, int lo=0, int hi=255) dict {
   "Return state with symbolic argv strings mapped as C strings.
   Each spec may be an int length or {name,n/len,lo,hi}."
   mut out = st
   def sol = state_solver(out)
   mut records = []
   mut sy = out.get("symbolics", dict())
   def ptr_base = base
   mut str_base = base + 0x1000
   def psz = _ptr_size(st)
   mut i = 0
   while i < specs.len {
      def spec = specs[i]
      def n = _argv_spec_len(spec)
      def nm = _argv_spec_name(spec, i)
      def xs = bytes_symbolic(sol, nm, n)
      def slo = _argv_spec_lo(spec, lo)
      def shi = _argv_spec_hi(spec, hi)
      if slo > 0 || shi < 255 { byte_constraints_ascii(sol, xs, slo, shi) }
      out = mem_write_bytes(out, str_base, xs)
      out = mem_write_byte(out, str_base + n, 0)
      out = _mem_write_ptr(out, ptr_base + i * psz, str_base)
      def rec = {"index": i, "addr": str_base, "name": nm, "ast": xs, "count": n,
         "ptr_addr": ptr_base + i * psz, "symbolic": true}
      records = records.append(rec)
      sy = sy.set(nm, rec).set(to_str(str_base), rec)
      str_base += n + 1
      i += 1
   }
   out = _mem_write_ptr(out, ptr_base + specs.len * psz, 0)
   out = out.set("solver", sol).set("symbolics", sy).set("argv", records).set("argc", specs.len).set("argv_addr", ptr_base)
   _state_set_argv_regs(out, specs.len, ptr_base)
}

fn state_eval_argv_ascii(dict st, int index) any {
   "Evaluate argv[index] as ASCII for concrete or symbolic argv records."
   def args = state_argv(st)
   if index < 0 || index >= args.len { return nil }
   def rec = args[index]
   if rec.get("symbolic", false) { return eval_ascii(state_solver(st), rec.get("ast", [])) }
   if rec.contains("value") { return rec.get("value", "") }
   state_eval_mem_ascii(st, rec.get("addr", 0))
}

fn _argv_symbolic_record(dict st, int index) dict {
   def args = state_argv(st)
   if index < 0 || index >= args.len { return dict() }
   def rec = args[index]
   rec.get("symbolic", false) ? rec : dict()
}

fn state_constrain_argv_eq(dict st, int index, any bytes) dict {
   "Constrain symbolic argv[index] to concrete bytes."
   def rec = _argv_symbolic_record(st, index)
   if rec.len == 0 { return st }
   def sol = state_solver(st)
   def cs = _byte_eq_constraints(sol, rec.get("ast", []), bytes)
   _assert_all(sol, cs)
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_eq_note("argv", "argv[" + to_str(index) + "]", bytes, cs.len))
}

fn state_constrain_argv_range(dict st, int index, int lo, int hi) dict {
   "Constrain symbolic argv[index] bytes to an inclusive unsigned range."
   def rec = _argv_symbolic_record(st, index)
   if rec.len == 0 { return st }
   def sol = state_solver(st)
   def cs = _byte_range_constraints(sol, rec.get("ast", []), lo, hi)
   _assert_all(sol, cs)
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_range_note("argv", "argv[" + to_str(index) + "]", lo, hi, rec.get("ast", []).len))
}

fn state_set_env(dict st, any env) dict {
   "Return state with environment metadata recorded."
   st.set("env", is_dict(env) ? env : dict())
}

fn state_set_fs(dict st, any fs) dict {
   "Return state with a virtual filesystem for libc/syscall file reads."
   st.set("fs", is_dict(fs) ? fs : dict())
}

fn state_fs(dict st) dict {
   "Return the state's virtual filesystem map."
   st.get("fs", dict())
}

fn state_set_net(dict st, any net) dict {
   "Return state with virtual network input/output metadata."
   st.set("net", is_dict(net) ? net : {"recv": net})
}

fn state_net(dict st) dict {
   "Return the state's virtual network map."
   st.get("net", dict())
}

fn state_set_time(dict st, any sec, any nsec=0) dict {
   "Return state with deterministic virtual wall-clock time."
   st.set("time", int(sec)).set("time_nsec", int(nsec))
}

fn state_set_random(dict st, any data) dict {
   "Return state with deterministic entropy consumed by getrandom/rand models."
   st.set("random", data).set("random_pos", 0)
}

fn state_maps(dict st) list {
   "Return tracked virtual memory mappings."
   st.get("maps", [])
}

fn state_process(dict st) dict {
   "Return process-facing state fields used by harnesses and UI tools."
   {"argc": st.get("argc", 0), "argv_addr": st.get("argv_addr", 0), "argv": state_argv(st),
      "env": state_env(st), "fs": state_fs(st), "maps": state_maps(st), "brk": int(st.get("brk", 0)),
      "auxv": st.get("auxv", dict()), "dl_handles": st.get("dl_handles", dict()),
      "net": state_net(st), "net_tx": st.get("net_tx", ""), "stdin": state_stdin(st),
      "pid": int(st.get("pid", 0)), "tid": int(st.get("tid", st.get("pid", 0))), "ppid": int(st.get("ppid", 0)),
      "uid": int(st.get("uid", 0)), "gid": int(st.get("gid", 0)),
      "tls_fs": int(st.get("tls_fs", 0)), "tls_gs": int(st.get("tls_gs", 0)),
      "fork_pid": int(st.get("fork_pid", 0)),
      "time": int(st.get("time", 0)), "time_nsec": int(st.get("time_nsec", 0)),
      "random_pos": int(st.get("random_pos", 0)),
      "stdout": state_stdout(st), "stderr": state_stderr(st)}
}

fn state_feasible(dict st) bool {
   "Return false when the state's solver proves its path constraints UNSAT.
   UNKNOWN is treated as feasible so exploration does not drop paths merely
   because the backend cannot decide them."
   if st.get("constraints", []).len == 0 { return true }
   def sol = state_solver(st)
   if !sol.get("ok", false) { return true }
   solver_check(sol) != UNSAT
}

fn state_set_addr(dict st, int addr) dict {
   "Return state with a new instruction address."
   st.set("addr", addr)
}

fn state_set_reg(dict st, any name, any value) dict {
   "Return state with register `name` set to `value`."
   def regs = _dict_copy(st.get("regs", dict())).set(to_str(name), value)
   st.set("regs", regs)
}

fn state_get_reg(dict st, any name, any default=0) any {
   "Read register `name` from state."
   st.get("regs", dict()).get(to_str(name), default)
}

fn mem_write(dict st, any addr, any data) dict {
   "Return state with concrete/symbolic bytes stored at `addr`."
   st.set("mem", _dict_copy(st.get("mem", dict())).set(int(addr), data))
}

fn _list_from_data(any data) list {
   if is_list(data) { return _list_copy(data) }
   mut out = []
   if is_str(data) || is_bytes(data) {
      mut i = 0
      while i < data.len { out = out.append(load8(data, i) & 255) i += 1 }
   }
   out
}

fn _items_from_data(any data) list {
   if is_list(data) { return _list_copy(data) }
   if is_int(data) { return [int(data) & 255] }
   _list_from_data(data)
}

fn mem_write_byte(dict st, any addr, any value) dict {
   "Return state with one byte/AST written at `addr`, patching mapped ranges when possible."
   def a = int(addr)
   def mem = _dict_copy(st.get("mem", dict()))
   if mem.contains(a) { return st.set("mem", mem.set(a, value)) }
   def ks = mem.keys()
   mut i = 0
   while i < ks.len {
      def base = int(ks[i])
      def data = mem.get(ks[i], "")
      def n = _data_len(data)
      if n > 0 && a >= base && a < base + n {
         mut xs = _items_from_data(data)
         xs = xs.set(a - base, value)
         return st.set("mem", mem.set(base, xs))
      }
      i += 1
   }
   st.set("mem", mem.set(a, value))
}

fn mem_write_bytes(dict st, any addr, any data) dict {
   "Return state with concrete/symbolic bytes written starting at `addr`."
   def xs = _items_from_data(data)
   mut out = st
   mut i = 0
   while i < xs.len {
      out = mem_write_byte(out, int(addr) + i, xs[i])
      i += 1
   }
   out
}

fn mem_read(dict st, any addr, any default=0) any {
   "Read memory entry from state."
   def mem = st.get("mem", dict())
   def a = int(addr)
   if mem.contains(a) { return mem.get(a, default) }
   def ks = mem.keys()
   mut i = 0
   while i < ks.len {
      def base = int(ks[i])
      def data = mem.get(ks[i], "")
      def n = _data_len(data)
      if n > 0 && a >= base && a < base + n { return _item_at(data, a - base) }
      i += 1
   }
   default
}

fn mem_read_bytes(dict st, any addr, int n) str {
   "Read `n` concrete bytes from mapped state memory."
   if n <= 0 { return "" }
   def mem = st.get("mem", dict())
   def a = int(addr)
   def ks = mem.keys()
   mut ki = 0
   while ki < ks.len {
      def base = int(ks[ki])
      def data = mem.get(ks[ki], "")
      def size = _data_len(data)
      if size > 0 && a >= base && a + n <= base + size {
         return _bytestr_slice(data, a - base, n)
      }
      ki += 1
   }
   mut out = str.Builder(n)
   mut i = 0
   while i < n {
      def b = mem_read(st, int(addr) + i, -1)
      if !is_int(b) || int(b) < 0 { break }
      out = str.builder_append_byte(out, int(b) & 255)
      i += 1
   }
   def s = str.builder_to_str(out)
   str.builder_free(out)
   s
}

fn _mem_read_byteish(dict st, any addr, any default=0) any {
   def v = mem_read(st, addr, default)
   if is_list(v) && v.len > 0 { return v[0] }
   if (is_str(v) || is_bytes(v)) && v.len > 0 { return load8(v, 0) & 255 }
   v
}

fn state_solver(dict st) dict {
   "Return the state's solver record."
   if st.contains("solver") { return st.get("solver") }
   solver()
}

fn state_add(dict st, any ast) dict {
   "Assert a path constraint into the state's solver and record it."
   def sol = state_solver(st)
   solver_add(sol, ast)
   st.set("solver", sol).set("constraints", _list_copy(st.get("constraints", [])).append(ast))
}

fn _byte_range_constraints(dict sol, list xs, int lo, int hi) list {
   if !sol.get("ok", false) { return [] }
   mut low = lo
   mut high = hi
   if low > high {
      def tmp = low
      low = high
      high = tmp
   }
   def ctx = sol.get("ctx", 0)
   mut out = []
   mut i = 0
   while i < xs.len {
      out = out.append(smt.bvuge(ctx, xs[i], smt.bv_u8(ctx, low)))
      out = out.append(smt.bvule(ctx, xs[i], smt.bv_u8(ctx, high)))
      i += 1
   }
   out
}

fn _assert_all(dict sol, list constraints) int {
   if !sol.get("ok", false) { return 0 }
   mut i = 0
   while i < constraints.len {
      if _is_ast(constraints[i]) { smt.solver_assert(sol.get("ctx", 0), sol.get("solver", 0), constraints[i]) }
      i += 1
   }
   constraints.len
}

fn _state_assert_constraints(dict st, list constraints) dict {
   def sol = state_solver(st)
   _assert_all(sol, constraints)
   st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), constraints))
}

fn _constraints_append_all(list base, list extra) list {
   mut out = _list_copy(base)
   mut i = 0
   while i < extra.len {
      out = out.append(extra[i])
      i += 1
   }
   out
}

fn _constraint_note(dict st, dict note) dict {
   st.set("constraint_notes", _list_copy(st.get("constraint_notes", [])).append(note))
}

fn _constraint_range_note(str kind, str target, int lo, int hi, int count=0) dict {
   {"kind": kind, "target": target, "op": "range", "lo": lo, "hi": hi,
      "count": count, "text": target + " in [" + to_str(lo) + ", " + to_str(hi) + "]"}
}

fn _constraint_eq_note(str kind, str target, any value, int count=0) dict {
   {"kind": kind, "target": target, "op": "eq", "value": value, "count": count,
      "text": target + " == " + repr(value)}
}

fn state_symbolic_bytes(dict st, str name, int n, int lo=0, int hi=255) dict {
   "Attach symbolic bytes to a state, optionally range-constrained."
   def sol = state_solver(st)
   def xs = bytes_symbolic(sol, name, n)
   mut constraints = _list_copy(st.get("constraints", []))
   if lo > 0 || hi < 255 {
      def cs = _byte_range_constraints(sol, xs, lo, hi)
      _assert_all(sol, cs)
      constraints = _constraints_append_all(constraints, cs)
   }
   st.set("solver", sol).set("constraints", constraints).set("symbolics", st.get("symbolics", dict()).set(name, xs))
}

fn state_symbolic_mem(dict st, any addr, int n, str name="", int lo=0, int hi=255) dict {
   "Attach symbolic bytes to memory at `addr` and record them in state symbolics."
   def a = int(addr)
   def nm = name.len > 0 ? name : ("mem_" + to_hex(a, 0))
   def sol = state_solver(st)
   def xs = bytes_symbolic(sol, nm, n)
   mut constraints = _list_copy(st.get("constraints", []))
   if lo > 0 || hi < 255 {
      def cs = _byte_range_constraints(sol, xs, lo, hi)
      _assert_all(sol, cs)
      constraints = _constraints_append_all(constraints, cs)
   }
   def rec = {"ast": xs, "bits": 8, "count": n, "kind": "mem", "addr": a, "name": nm}
   mut sy = st.get("symbolics", dict()).set(nm, rec)
   mut bi = 0
   while bi < n {
      sy = sy.set(to_str(a + bi), rec)
      bi += 1
   }
   mem_write(st.set("solver", sol).set("constraints", constraints).set("symbolics", sy), a, xs)
}

fn _symbolic_mem_record(dict st, any addr) dict {
   def a = int(addr)
   def sy = st.get("symbolics", dict())
   if is_dict(sy.get(to_str(a), dict())) { return sy.get(to_str(a), dict()) }
   def keys = sy.keys()
   mut i = 0
   while i < keys.len {
      def r = sy.get(keys[i], dict())
      if is_dict(r) && r.get("kind", "") == "mem" {
         def base = int(r.get("addr", -1))
         def count = int(r.get("count", 0))
         if a >= base && a < base + count { return r }
      }
      i += 1
   }
   dict()
}

fn _first_symbolic_mem_addr(dict st) int {
   def sy = st.get("symbolics", dict())
   def keys = sy.keys()
   mut i = 0
   while i < keys.len {
      def r = sy.get(keys[i], dict())
      if is_dict(r) && r.get("kind", "") == "mem" { return int(r.get("addr", 0)) }
      i += 1
   }
   0
}

fn _symbolic_candidate_addr(dict st, int addr) int {
   if addr > 0 { return addr }
   _first_symbolic_mem_addr(st)
}

fn _mem_has_byte(dict st, int addr) bool {
   def missing = "__ny_mem_missing__"
   def v = mem_read(st, addr, missing)
   !(is_str(v) && v == missing)
}

fn _symbolic_compare_addr(dict st, int addr) int {
   if addr != 0 && _mem_has_byte(st, addr) { return addr }
   def last = int(st.get("last_input_dst", 0))
   if last != 0 && _mem_has_byte(st, last) { return last }
   _symbolic_candidate_addr(st, addr)
}

fn _symbolic_mem_byte_ast(dict st, any addr) any {
   def a = int(addr)
   def r = _symbolic_mem_record(st, a)
   if r.len == 0 { return nil }
   def base = int(r.get("addr", -1))
   def off = a - base
   def xs = r.get("ast", [])
   if off < 0 || off >= xs.len { return nil }
   xs[off]
}

fn _memory_byte_ast(dict st, any addr) any {
   mut v = mem_read(st, int(addr), nil)
   if is_list(v) && v.len > 0 { v = v[0] }
   if _is_ast(v) { return v }
   _symbolic_mem_byte_ast(st, addr)
}

fn state_constrain_mem_eq(dict st, any addr, any bytes) dict {
   "Constrain symbolic memory bytes at `addr` to concrete bytes."
   def sol = state_solver(st)
   if !sol.get("ok", false) { return st }
   def ctx = sol.get("ctx", 0)
   mut xs = []
   mut cs = []
   def base_addr = int(addr)
   mut i = 0
   while i < _data_len(bytes) {
      def ast = _memory_byte_ast(st, base_addr + i)
      if ast != nil { xs = xs.append(ast) }
      if ast != nil {
         cs = cs.append(smt.mk_eq(ctx, ast, smt.bv_u8(ctx, _item_at(bytes, i))))
      }
      i += 1
   }
   if xs.len > 0 { smt.solver_assert_bytes_eq(ctx, sol.get("solver", 0), xs, bytes) }
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_eq_note("mem", "mem[" + to_hex(base_addr, 0) + "]", bytes, cs.len))
}

fn state_constrain_mem_range(dict st, any addr, int n, int lo, int hi) dict {
   "Constrain `n` symbolic memory bytes at `addr` to an inclusive unsigned range."
   def sol = state_solver(st)
   if !sol.get("ok", false) { return st }
   mut low = lo
   mut high = hi
   if low > high {
      def tmp = low
      low = high
      high = tmp
   }
   def ctx = sol.get("ctx", 0)
   mut cs = []
   mut i = 0
   while i < n {
      def ast = _memory_byte_ast(st, int(addr) + i)
      if _is_ast(ast) {
         cs = cs.append(smt.bvuge(ctx, ast, smt.bv_u8(ctx, low)))
         cs = cs.append(smt.bvule(ctx, ast, smt.bv_u8(ctx, high)))
      }
      i += 1
   }
   _assert_all(sol, cs)
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_range_note("mem", "mem[" + to_hex(int(addr), 0) + "]", low, high, n))
}

fn state_eval_mem_byte_u64(dict st, any addr) any {
   "Evaluate one concrete or symbolic byte at `addr` as u64."
   mut v = mem_read(st, addr, nil)
   if v == nil { return nil }
   if is_list(v) && v.len > 0 { v = v[0] }
   if is_int(v) { return int(v) & 255 }
   eval_u64(state_solver(st), v)
}

fn state_eval_mem_bytes(dict st, any addr) any {
   "Evaluate symbolic memory bytes stored exactly at `addr` as integers."
   def a = int(addr)
   def r = _symbolic_mem_record(st, a)
   if r.len > 0 && int(r.get("addr", -1)) == a {
      def xs = r.get("ast", [])
      mut out = []
      mut i = 0
      while i < xs.len {
         def v = eval_u64(state_solver(st), xs[i])
         out = out.append(v == nil ? 0 : (int(v) & 255))
         i += 1
      }
      return out
   }
   def data = mem_read(st, addr, nil)
   if data == nil { return nil }
   if is_list(data) { return data }
   if is_str(data) || is_bytes(data) {
      mut out = []
      mut i = 0
      while i < data.len { out = out.append(load8(data, i) & 255) i += 1 }
      return out
   }
   is_int(data) ? [data & 255] : nil
}

fn state_eval_mem_ascii(dict st, any addr) any {
   "Evaluate symbolic memory bytes stored exactly at `addr` as ASCII."
   def bs = state_eval_mem_bytes(st, addr)
   if !is_list(bs) { return nil }
   mut b = str.Builder(bs.len)
   mut i = 0
   while i < bs.len { b = str.builder_append_byte(b, int(bs[i]) & 255) i += 1 }
   def s = str.builder_to_str(b)
   str.builder_free(b)
   s
}

fn _reg_bits(dict st, str reg, int bits=0) int {
   if bits > 0 { return bits }
   def r = str.lower(reg)
   if r == "eax" || r == "ebx" || r == "ecx" || r == "edx" || r == "esi" || r == "edi" || r == "eip" { return 32 }
   if str.startswith(r, "w") && r.len > 1 { return 32 }
   if r == "ax" || r == "bx" || r == "cx" || r == "dx" || r == "si" || r == "di" { return 16 }
   if r == "al" || r == "bl" || r == "cl" || r == "dl" || r == "ah" || r == "bh" || r == "ch" || r == "dh" ||
   r == "sil" || r == "dil" || r == "bpl" || r == "spl" { return 8 }
   def ai = st.get("arch", dict())
   is_dict(ai) ? int(ai.get("bits", 64)) : 64
}

fn state_symbolic_reg(dict st, str reg, str name="", int bits=0) dict {
   "Attach a symbolic bitvector to register `reg` and record it in state symbolics."
   def r = str.lower(reg)
   def b = _reg_bits(st, r, bits)
   def sol = state_solver(st)
   def nm = name.len > 0 ? name : r
   def ast = bvs(sol, nm, b)
   state_set_reg(st.set("solver", sol).set("symbolics", st.get("symbolics", dict()).set(r, {"ast": ast, "bits": b, "kind": "reg", "name": nm})), r, ast)
}

fn state_constrain_reg_eq(dict st, str reg, any value) dict {
   "Constrain a symbolic register to one concrete value."
   def r = str.lower(reg)
   def rec = st.get("symbolics", dict()).get(r, dict())
   if !is_dict(rec) || rec.get("kind", "") != "reg" { return st }
   def sol = state_solver(st)
   if !sol.get("ok", false) { return st }
   def bits = int(rec.get("bits", _reg_bits(st, r)))
   def ctx = sol.get("ctx", 0)
   def c = smt.mk_eq(ctx, rec.get("ast", 0), smt.bv_u64(ctx, value, bits))
   _assert_all(sol, [c])
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), [c])),
      _constraint_eq_note("reg", r, value, 1))
}

fn state_constrain_reg_range(dict st, str reg, int lo, int hi) dict {
   "Constrain a symbolic register to an inclusive unsigned range."
   def r = str.lower(reg)
   def rec = st.get("symbolics", dict()).get(r, dict())
   if !is_dict(rec) || rec.get("kind", "") != "reg" { return st }
   def sol = state_solver(st)
   if !sol.get("ok", false) { return st }
   mut low = lo
   mut high = hi
   if low > high {
      def tmp = low
      low = high
      high = tmp
   }
   def bits = int(rec.get("bits", _reg_bits(st, r)))
   def ctx = sol.get("ctx", 0)
   def ast = rec.get("ast", 0)
   def c0 = smt.bvuge(ctx, ast, smt.bv_u64(ctx, low, bits))
   def c1 = smt.bvule(ctx, ast, smt.bv_u64(ctx, high, bits))
   _assert_all(sol, [c0, c1])
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), [c0, c1])),
      _constraint_range_note("reg", r, low, high, 1))
}

fn _symbolic_named(dict st, str name) any {
   def sy = st.get("symbolics", dict())
   if sy.contains(name) { return sy.get(name, nil) }
   def keys = sy.keys()
   mut i = 0
   while i < keys.len {
      def v = sy.get(keys[i], nil)
      if is_dict(v) && v.get("name", "") == name { return v }
      i += 1
   }
   nil
}

fn _symbolic_ast_list(any rec) list {
   if is_list(rec) { return rec }
   if is_dict(rec) {
      def ast = rec.get("ast", nil)
      if is_list(ast) { return ast }
   }
   []
}

fn state_constrain_symbolic_eq(dict st, str name, any value, int offset=-1) dict {
   "Constrain a named symbolic byte vector, memory record, argv record, or register.
   The name may be the public symbolic name or the register key that owns it."
   def rec = _symbolic_named(st, name)
   if rec == nil { return st }
   def sol = state_solver(st)
   if !sol.get("ok", false) { return st }
   def ctx = sol.get("ctx", 0)
   mut cs = []
   if is_dict(rec) && rec.get("kind", "") == "reg" {
      def bits = int(rec.get("bits", 64))
      cs = [smt.mk_eq(ctx, rec.get("ast", 0), smt.bv_u64(ctx, value, bits))]
   } else {
      def xs0 = _symbolic_ast_list(rec)
      mut xs = []
      if offset >= 0 {
         if offset < xs0.len { xs = [xs0[offset]] }
      } else {
         xs = xs0
      }
      cs = _byte_eq_constraints(sol, xs, value)
   }
   _assert_all(sol, cs)
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_eq_note("symbolic", name, value, cs.len))
}

fn state_constrain_symbolic_range(dict st, str name, int lo, int hi, int offset=0, int n=0) dict {
   "Constrain a named symbolic byte vector, memory record, argv record, or register
   to an inclusive unsigned range."
   def rec = _symbolic_named(st, name)
   if rec == nil { return st }
   def sol = state_solver(st)
   if !sol.get("ok", false) { return st }
   def ctx = sol.get("ctx", 0)
   mut cs = []
   if is_dict(rec) && rec.get("kind", "") == "reg" {
      mut low = lo
      mut high = hi
      if low > high {
         def tmp = low
         low = high
         high = tmp
      }
      def bits = int(rec.get("bits", 64))
      def ast = rec.get("ast", 0)
      cs = [smt.bvuge(ctx, ast, smt.bv_u64(ctx, low, bits)), smt.bvule(ctx, ast, smt.bv_u64(ctx, high, bits))]
   } else {
      def xs0 = _symbolic_ast_list(rec)
      mut xs = []
      def start = max(0, offset)
      def count = n <= 0 ? xs0.len - start : n
      mut i = 0
      while i < count && start + i < xs0.len {
         xs = xs.append(xs0[start + i])
         i += 1
      }
      cs = _byte_range_constraints(sol, xs, lo, hi)
   }
   _assert_all(sol, cs)
   _constraint_note(st.set("solver", sol).set("constraints", _constraints_append_all(st.get("constraints", []), cs)),
      _constraint_range_note("symbolic", name, lo, hi, cs.len))
}

fn state_eval_reg_u64(dict st, str reg) any {
   "Evaluate a symbolic/concrete register as u64 in the state's solver."
   def v = state_get_reg(st, reg, nil)
   if v == nil { return nil }
   if is_int(v) { return v }
   eval_u64(state_solver(st), v)
}

fn state_eval_reg_hex(dict st, str reg) any {
   "Evaluate a symbolic/concrete register as a fixed-width hex string."
   def r = str.lower(reg)
   def v = state_get_reg(st, r, nil)
   if v == nil { return nil }
   def bits = is_dict(st.get("symbolics", dict()).get(r, dict())) ? int(st.get("symbolics", dict()).get(r, dict()).get("bits", _reg_bits(st, r))) : _reg_bits(st, r)
   if is_int(v) { return smt.hex_width(to_hex(int(v), 0), bits) }
   eval_hex(state_solver(st), v, bits)
}

fn state_eval_ascii(dict st, str name) any {
   "Evaluate a named symbolic byte vector from a state as ASCII."
   def xs = _symbolic_ast_list(_symbolic_named(st, name))
   eval_ascii(state_solver(st), xs)
}

fn state_clone(dict st) dict {
   "Clone the mutable structural parts of a state. Solver/context handles are
   shared, but path-local registers, memory, history, and constraint lists are
   copied so forked paths cannot corrupt one another."
   mut out = _dict_copy(st)
   out = out.set("regs", _dict_copy(st.get("regs", dict())))
   out = out.set("mem", _dict_copy(st.get("mem", dict())))
   out = out.set("symbolics", _dict_copy(st.get("symbolics", dict())))
   out = out.set("constraints", _list_copy(st.get("constraints", [])))
   out = out.set("constraint_notes", _list_copy(st.get("constraint_notes", [])))
   out = out.set("path_conditions", _list_copy(st.get("path_conditions", [])))
   out = out.set("history", _list_copy(st.get("history", [])))
   out = out.set("callstack", _list_copy(st.get("callstack", [])))
   out = out.set("maps", _list_copy(st.get("maps", [])))
   out = out.set("argv", _list_copy(st.get("argv", [])))
   out = out.set("env", _dict_copy(st.get("env", dict())))
   out = out.set("fs", _dict_copy(st.get("fs", dict())))
   out = out.set("net", _dict_copy(st.get("net", dict())))
   out
}

fn state_fork(dict st, any true_constraint=0, any false_constraint=0) list {
   "Create two state records and append branch metadata. Solver handles are
   intentionally shared in this lightweight layer ; use fresh solvers for heavy
   path independence."
   mut t = state_clone(st).set("branch", "true")
   mut f = state_clone(st).set("branch", "false")
   if true_constraint { t = t.set("constraints", _list_copy(t.get("constraints", [])).append(true_constraint)) }
   if false_constraint { f = f.set("constraints", _list_copy(f.get("constraints", [])).append(false_constraint)) }
   [t, f]
}

fn _list_copy(list xs) list {
   slice(xs, 0, xs.len, 1)
}

fn _append_unique(list xs, any value) list {
   mut i = 0
   while i < xs.len {
      if xs[i] == value { return xs }
      i += 1
   }
   xs = xs.append(value)
   xs
}

fn _dict_copy(dict d) dict {
   mut out = dict()
   def ks = d.keys()
   mut i = 0
   while i < ks.len {
      out = out.set(ks[i], d.get(ks[i], nil))
      i += 1
   }
   out
}

fn _history_append(dict st, dict ev) dict {
   st.set("history", _list_copy(st.get("history", [])).append(ev))
}

fn _project_options(any proj) dict {
   is_dict(proj) ? proj.get("options", dict()) : dict()
}

fn _project_blob(any proj) any {
   _project_options(proj).get("blob", "")
}

fn _project_blob_at(any proj, int pc) list {
   def loads = is_dict(proj) ? project_loads(proj) : []
   mut i = 0
   while i < loads.len {
      def ld = loads[i]
      def base = int(ld.get("addr", 0))
      def size = int(ld.get("size", _data_len(ld.get("data", ""))))
      if size > 0 && pc >= base && pc < base + size {
         return [ld.get("data", ""), base]
      }
      i += 1
   }
   [_project_blob(proj), _project_base(proj)]
}

fn _project_base(any proj) int {
   int(is_dict(proj) ? proj.get("base", _project_options(proj).get("base", 0)) : 0)
}

fn _project_arch(any proj) str {
   if !is_dict(proj) { return "unknown" }
   def ai = proj.get("arch", dict())
   if is_dict(ai) { return ai.get("arch", "unknown") }
   to_str(ai)
}

fn _project_hook_at(any proj, int addr) dict {
   if !is_dict(proj) { return dict() }
   project_hooks(proj).get(to_str(addr), dict())
}

fn _state_map_project_loads(dict st, any proj) dict {
   if !is_dict(proj) { return st }
   def loads = project_loads(proj)
   mut out = st
   mut i = 0
   while i < loads.len {
      def ld = loads[i]
      def data = ld.get("data", "")
      if _data_len(data) > 0 {
         out = mem_write(out, int(ld.get("addr", 0)), data)
      }
      i += 1
   }
   out.set("mapped_loads", loads.len)
}

fn _byte_at(any data, int idx) int {
   if idx < 0 || idx >= _data_len(data) { return -1 }
   if is_list(data) { return int(data[idx]) & 255 }
   load8(_bytestr(data), idx) & 255
}

fn _item_at(any data, int idx) any {
   if idx < 0 || idx >= _data_len(data) { return nil }
   if is_list(data) { return data[idx] }
   load8(_bytestr(data), idx) & 255
}

fn _bytestr_slice(any data, int off, int n) str {
   if off < 0 || n <= 0 { return "" }
   mut out = str.Builder(n)
   mut i = 0
   while i < n && off + i < _data_len(data) {
      out = str.builder_append_byte(out, _byte_at(data, off + i))
      i += 1
   }
   def s = str.builder_to_str(out)
   str.builder_free(out)
   s
}

fn _state_set_pc(dict st, int addr) dict {
   state_set_reg(state_set_addr(st, addr), _abi_profile(st.get("arch", dict())).get("pc", "rip"), addr)
}

fn _i8(int v) int {
   def x = v & 255
   x >= 128 ? x - 256 : x
}

fn _i32le(any data, int off) int {
   if _byte_at(data, off + 3) < 0 { return 0 }
   def x = (_byte_at(data, off) & 255) | ((_byte_at(data, off + 1) & 255) << 8) |
   ((_byte_at(data, off + 2) & 255) << 16) | ((_byte_at(data, off + 3) & 255) << 24)
   x >= 0x80000000 ? x - 0x100000000 : x
}

fn _u32le_any(any data, int off) int {
   if _byte_at(data, off + 3) < 0 { return 0 }
   (_byte_at(data, off) & 255) | ((_byte_at(data, off + 1) & 255) << 8) |
   ((_byte_at(data, off + 2) & 255) << 16) | ((_byte_at(data, off + 3) & 255) << 24)
}

fn _u64le_any(any data, int off) int {
   if _byte_at(data, off + 7) < 0 { return 0 }
   _u32le_any(data, off) | (_u32le_any(data, off + 4) << 32)
}

fn _mov_imm_reg32(int op) str {
   match op {
      0xb8 -> "eax"
      0xb9 -> "ecx"
      0xba -> "edx"
      0xbb -> "ebx"
      0xbc -> "esp"
      0xbd -> "ebp"
      0xbe -> "esi"
      0xbf -> "edi"
      _ -> ""
   }
}

fn _mov_imm_reg64(int op) str {
   match op {
      0xb8 -> "rax"
      0xb9 -> "rcx"
      0xba -> "rdx"
      0xbb -> "rbx"
      0xbc -> "rsp"
      0xbd -> "rbp"
      0xbe -> "rsi"
      0xbf -> "rdi"
      _ -> ""
   }
}

fn _reg32_to_64(str reg) str {
   match reg {
      "eax" -> "rax"
      "ecx" -> "rcx"
      "edx" -> "rdx"
      "ebx" -> "rbx"
      "esp" -> "rsp"
      "ebp" -> "rbp"
      "esi" -> "rsi"
      "edi" -> "rdi"
      _ -> reg
   }
}

fn _base_reg_for_byte_mem(int modrm) str {
   match modrm {
      0x06 -> "rsi"
      0x07 -> "rdi"
      0x46 -> "rsi"
      0x47 -> "rdi"
      0x3e -> "rsi"
      0x3f -> "rdi"
      0x7e -> "rsi"
      0x7f -> "rdi"
      _ -> ""
   }
}

fn _byte_mem_has_disp(int modrm) bool {
   modrm == 0x46 || modrm == 0x47 || modrm == 0x7e || modrm == 0x7f
}

fn _byte_mem_ptr(dict st, int modrm, int disp) int {
   def base = _base_reg_for_byte_mem(modrm)
   base.len > 0 ? int(state_get_reg(st, base, 0)) + disp : 0
}

fn _describe_byte_mem(str base, int disp) str {
   "byte ptr [" + base + (disp == 0 ? "" : ((disp > 0 ? "+" : "") + to_str(disp))) + "]"
}

fn _byte_reg_value(dict st, int reg_id) any {
   match reg_id {
      0 -> state_get_reg(st, "al", state_get_reg(st, "rax", state_get_reg(st, "eax", 0)))
      1 -> state_get_reg(st, "cl", state_get_reg(st, "rcx", state_get_reg(st, "ecx", 0)))
      2 -> state_get_reg(st, "dl", state_get_reg(st, "rdx", state_get_reg(st, "edx", 0)))
      3 -> state_get_reg(st, "bl", state_get_reg(st, "rbx", state_get_reg(st, "ebx", 0)))
      _ -> 0
   }
}

fn _byte_reg_name(int reg_id) str {
   match reg_id {
      0 -> "al"
      1 -> "cl"
      2 -> "dl"
      3 -> "bl"
      _ -> "r8b"
   }
}

fn _low8(any v) int {
   is_int(v) ? (int(v) & 255) : 0
}

fn _set_al_value(dict st, any value) dict {
   mut out = state_set_reg(st, "al", value)
   if _is_ast(value) {
      def sol = state_solver(out)
      out = state_set_reg(out, "eax", smt.bvzext(sol.get("ctx", 0), value, 24))
      out = state_set_reg(out, "rax", smt.bvzext(sol.get("ctx", 0), value, 56))
   } else {
      out = state_set_reg(out, "eax", _low8(value))
      out = state_set_reg(out, "rax", _low8(value))
   }
   out
}

fn _set_eax_from_byte(dict st, any value, bool signed=false) dict {
   mut out = state_set_reg(st, "al", value)
   if _is_ast(value) {
      def sol = state_solver(out)
      out = state_set_reg(out, "eax", signed ? smt.bvsext(sol.get("ctx", 0), value, 24) : smt.bvzext(sol.get("ctx", 0), value, 24))
      out = state_set_reg(out, "rax", signed ? smt.bvsext(sol.get("ctx", 0), value, 56) : smt.bvzext(sol.get("ctx", 0), value, 56))
   } else {
      def b = _low8(value)
      def v = signed && b >= 128 ? b - 256 : b
      out = state_set_reg(out, "eax", v)
      out = state_set_reg(out, "rax", v)
   }
   out
}

fn _al_imm8_ast(dict st, any alv, str op, int imm8) any {
   def sol = state_solver(st)
   def rhs = smt.bv_u8(sol.get("ctx", 0), imm8)
   match op {
      "xor" -> smt.bvxor(sol.get("ctx", 0), alv, rhs)
      "add" -> smt.bvadd(sol.get("ctx", 0), alv, rhs)
      "sub" -> smt.bvsub(sol.get("ctx", 0), alv, rhs)
      _ -> alv
   }
}

fn _al_imm8_int(any alv, str op, int imm8) int {
   def a = _low8(alv)
   match op {
      "xor" -> (a ^^ imm8) & 255
      "add" -> (a + imm8) & 255
      "sub" -> (a - imm8) & 255
      _ -> a
   }
}

fn _apply_al_imm8(dict st, str op, int imm8) dict {
   def alv = state_get_reg(st, "al", state_get_reg(st, "rax", state_get_reg(st, "eax", 0)))
   mut out = _is_ast(alv) ? _set_al_value(st, _al_imm8_ast(st, alv, op, imm8)) : _set_al_value(st, _al_imm8_int(alv, op, imm8))
   _set_zf(out, !_is_ast(state_get_reg(out, "al", 0)) && _low8(state_get_reg(out, "al", 0)) == 0)
}

fn _apply_al_bitmask_imm8(dict st, int imm8) dict {
   def alv = state_get_reg(st, "al", state_get_reg(st, "rax", state_get_reg(st, "eax", 0)))
   if _is_ast(alv) {
      def sol = state_solver(st)
      def masked = smt.bvand(sol.get("ctx", 0), alv, smt.bv_u8(sol.get("ctx", 0), imm8))
      return _set_zf_ast(st, smt.mk_eq(sol.get("ctx", 0), masked, smt.bv_u8(sol.get("ctx", 0), 0)), "(al & " + to_str(imm8) + ") == 0")
   }
   _set_zf_cmp(st, (_low8(alv) & imm8) == 0, "(al & " + to_str(imm8) + ") == 0")
}

fn _apply_eax_zero_check(dict st) dict {
   def eaxv = state_get_reg(st, "eax", state_get_reg(st, "rax", 0))
   if _is_ast(eaxv) {
      def sol = state_solver(st)
      def eqz = smt.mk_eq(sol.get("ctx", 0), eaxv, smt.bv_u32(sol.get("ctx", 0), 0))
      def proc_zero = _proc_return_zero_formula(st, "eax", eqz)
      return _set_zf_ast(st, _is_ast(proc_zero) ? proc_zero : eqz, "eax == 0")
   }
   _set_zf_cmp(st, (int(eaxv) & 0xffffffff) == 0, "eax == 0")
}

fn _call_push(dict st, int ret) dict {
   st.set("callstack", st.get("callstack", []).append(ret))
}

fn _call_pop(dict st) dict {
   def cs = st.get("callstack", [])
   if cs.len == 0 { return {"ok": false, "state": st, "ret": 0} }
   {"ok": true, "state": st.set("callstack", slice(cs, 0, cs.len - 1, 1)), "ret": cs[cs.len - 1]}
}

fn _is_ast(any x) bool {
   type(x) == "ffi_ptr" || type(x) == "fnptr"
}

fn _solver_with_constraints(dict base, list constraints) dict {
   if !base.get("ok", false) { return base }
   def ctx = base.get("ctx", 0)
   def s = smt.solver_new_for_logic(ctx, base.get("logic", "QF_BV"))
   if !s { return base }
   mut i = 0
   while i < constraints.len {
      if _is_ast(constraints[i]) { smt.solver_assert(ctx, s, constraints[i]) }
      i += 1
   }
   {"ok": true, "ctx": ctx, "solver": s, "logic": base.get("logic", "QF_BV"),
      "vars": base.get("vars", dict()), "borrow_ctx": true}
}

fn _set_zf(dict st, bool zf) dict {
   st.set("flags", st.get("flags", dict()).set("zf", zf))
}

fn _set_zf_cmp(dict st, bool zf, str expr) dict {
   _set_zf(st, zf).set("flag_expr", st.get("flag_expr", dict()).set("zf", expr))
}

fn _set_zf_ast(dict st, any ast, str expr) dict {
   st.set("flag_expr", st.get("flag_expr", dict()).set("zf", expr))
   .set("flag_ast", st.get("flag_ast", dict()).set("zf", ast))
}

fn _path_condition(dict st, str expr, bool value) dict {
   def rec = {"expr": expr, "value": value, "text": expr + " == " + to_str(value)}
   _constraint_note(st.set("path_conditions", _list_copy(st.get("path_conditions", [])).append(rec)),
      {"kind": "branch", "target": expr, "op": "path", "value": value, "count": 1, "text": rec.get("text", "")})
}

fn _branch_state(dict st, int pc, int next, str kind, bool taken) dict {
   def desc = kind + (taken ? " taken" : " fallthrough")
   def expr = st.get("flag_expr", dict()).get("zf", "zf")
   def want_zf = kind == "je" ? taken : !taken
   mut out = _path_condition(_state_set_pc(state_clone(st), next), expr, want_zf)
   def ast = st.get("flag_ast", dict()).get("zf", 0)
   if _is_ast(ast) {
      def sol = state_solver(st)
      def constraint = want_zf ? ast : smt.mk_not(sol.get("ctx", 0), ast)
      def constraints = _list_copy(out.get("constraints", [])).append(constraint)
      out = out.set("constraints", constraints).set("solver", _solver_with_constraints(sol, constraints))
   }
   _history_append(out.set("step_ok", true).set("step_reason", "ok").set("branch", taken ? "true" : "false"),
      {"addr": pc, "next": next, "backend": "lite_x86", "insn": desc, "branch": taken})
}

fn _hex_digit_value(int ch) int {
   if ch >= 48 && ch <= 57 { return ch - 48 }
   if ch >= 65 && ch <= 70 { return ch - 55 }
   if ch >= 97 && ch <= 102 { return ch - 87 }
   -1
}

fn _parse_lift_hex(str s) int {
   mut out = 0
   mut i = 0
   while i < s.len {
      def v = _hex_digit_value(load8(s, i))
      if v < 0 { break }
      out = (out << 4) | v
      i += 1
   }
   out
}

fn _parse_lift_imm(str raw) int {
   mut s = str.strip(raw)
   if str.startswith(s, "#") { s = str.strip(slice(s, 1, s.len, 1)) }
   mut sign = 1
   if str.startswith(s, "-") { sign = -1 s = str.strip(slice(s, 1, s.len, 1)) }
   if str.startswith(s, "0x") || str.startswith(s, "0X") { return sign * _parse_lift_hex(slice(s, 2, s.len, 1)) }
   if s.len == 0 { return 0 }
   sign * str.parse_int(s, 10)
}

fn _lift_reg_aliases(str reg) list {
   def r = str.lower(str.strip(reg))
   match r {
      "rax", "eax", "ax", "al" -> ["rax", "eax", "ax", "al"]
      "rbx", "ebx", "bx", "bl" -> ["rbx", "ebx", "bx", "bl"]
      "rcx", "ecx", "cx", "cl" -> ["rcx", "ecx", "cx", "cl"]
      "rdx", "edx", "dx", "dl" -> ["rdx", "edx", "dx", "dl"]
      "rsi", "esi", "si", "sil" -> ["rsi", "esi", "si", "sil"]
      "rdi", "edi", "di", "dil" -> ["rdi", "edi", "di", "dil"]
      "rbp", "ebp", "bp", "bpl" -> ["rbp", "ebp", "bp", "bpl"]
      "rsp", "esp", "sp", "spl" -> ["rsp", "esp", "sp", "spl"]
      "rip", "eip" -> ["rip", "eip"]
      _ -> {
         if str.startswith(r, "r") && str.endswith(r, "d") { return [slice(r, 0, r.len - 1, 1), r] }
         [r]
      }
   }
}

fn _lift_get_reg(dict st, str reg, any default=0) any {
   def regs = st.get("regs", dict())
   if regs.contains(reg) { return regs.get(reg, default) }
   def aliases = _lift_reg_aliases(reg)
   mut i = 0
   while i < aliases.len {
      if regs.contains(aliases[i]) {
         def v = regs.get(aliases[i], default)
         if _is_ast(v) {
            def want = _reg_bits(st, reg)
            def have = _reg_bits(st, aliases[i])
            if want > 0 && have > want {
               return smt.bv_extract(state_solver(st).get("ctx", 0), want - 1, 0, v)
            }
         }
         if is_int(v) {
            def bits = _reg_bits(st, reg)
            if bits == 8 { return int(v) & 255 }
            if bits == 16 { return int(v) & 0xffff }
            if bits == 32 { return int(v) & 0xffffffff }
         }
         return v
      }
      i += 1
   }
   default
}

fn _lift_operand_value(dict st, str op) any {
   def s = str.lower(str.strip(op))
   if s.len == 0 { return 0 }
   if str.startswith(s, "mem(") || str.startswith(s, "mem_fs(") || str.startswith(s, "mem_gs(") || str.find(s, "[") >= 0 || _lift_paren_mem_parts(s).len > 0 { return _lift_mem_load(st, s) }
   if s == "zero" || s == "x0" { return 0 }
   if str.startswith(s, "#") || str.startswith(s, "0x") || str.startswith(s, "-0x") || str.ascii_is_digit(load8(s, 0)) || load8(s, 0) == 45 { return _parse_lift_imm(s) }
   _lift_get_reg(st, op, 0)
}

fn _lift_paren_mem_parts(str op) dict {
   def s = str.strip(op)
   if str.startswith(s, "mem(") || str.startswith(s, "mem_fs(") || str.startswith(s, "mem_gs(") { return dict() }
   def open = str.find(s, "(")
   if open < 0 || !str.endswith(s, ")") { return dict() }
   def off = str.strip(slice(s, 0, open, 1))
   def base = str.strip(slice(s, open + 1, s.len - 1, 1))
   if base.len == 0 { return dict() }
   {"base": base, "offset": off.len == 0 ? "0" : off}
}

fn _lift_mem_inner(str op) str {
   mut s = str.strip(op)
   if str.startswith(s, "mem(") && str.endswith(s, ")") { return str.strip(slice(s, 4, s.len - 1, 1)) }
   if str.startswith(s, "mem_fs(") && str.endswith(s, ")") { return str.strip(slice(s, 7, s.len - 1, 1)) }
   if str.startswith(s, "mem_gs(") && str.endswith(s, ")") { return str.strip(slice(s, 7, s.len - 1, 1)) }
   def pm = _lift_paren_mem_parts(s)
   if pm.len > 0 {
      def off = pm.get("offset", "0")
      if off == "0" { return pm.get("base", "") }
      if str.startswith(off, "-") { return pm.get("base", "") + off }
      return pm.get("base", "") + "+" + off
   }
   mut a = -1
   mut b = -1
   mut i = 0
   while i < s.len {
      def ch = load8(s, i)
      if ch == 91 && a < 0 { a = i }
      elif ch == 93 { b = i }
      i += 1
   }
   if a >= 0 && b > a { return str.strip(slice(s, a + 1, b, 1)) }
   s
}

fn _lift_no_spaces(str s) str {
   mut b = str.Builder(s.len)
   mut i = 0
   while i < s.len {
      def ch = load8(s, i)
      if ch != 32 && ch != 9 { b = str.builder_append(b, str.chr(ch)) }
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _lift_mem_addr(dict st, str op) int {
   mut s = _lift_no_spaces(str.lower(_lift_mem_inner(op)))
   if s.len == 0 { return 0 }
   def av = _lift_address_value(st, op, "rax")
   if is_int(av) { return int(av) }
   if str.startswith(s, "0x") || str.ascii_is_digit(load8(s, 0)) { return _parse_lift_imm(s) }
   mut split = -1
   mut sign = 1
   mut i = 1
   while i < s.len {
      def ch = load8(s, i)
      if ch == 43 || ch == 45 { split = i sign = ch == 45 ? -1 : 1 break }
      i += 1
   }
   if split < 0 { return int(_lift_get_reg(st, s, 0)) }
   def base = slice(s, 0, split, 1)
   def off = slice(s, split + 1, s.len, 1)
   int(_lift_get_reg(st, base, 0)) + sign * _parse_lift_imm(off)
}

fn _lift_address_term(dict st, str term, str dst) any {
   def t = str.strip(term)
   if t.len == 0 { return 0 }
   def mul = str.find(t, "*")
   if mul >= 0 {
      def left = str.strip(slice(t, 0, mul, 1))
      def right = str.strip(slice(t, mul + 1, t.len, 1))
      def lv = _lift_address_term(st, left, dst)
      def rv = _lift_address_term(st, right, dst)
      if _is_ast(lv) || _is_ast(rv) {
         def sol = state_solver(st)
         return smt.bvmul(sol.get("ctx", 0), _lift_value_for_ast(st, dst, lv), _lift_value_for_ast(st, dst, rv))
      }
      return int(lv) * int(rv)
   }
   if str.startswith(t, "0x") || str.startswith(t, "-0x") || str.ascii_is_digit(load8(t, 0)) || load8(t, 0) == 45 {
      return _parse_lift_imm(t)
   }
   _lift_get_reg(st, t, 0)
}

fn _lift_address_value(dict st, str op, str dst="rax") any {
   def s0 = _lift_no_spaces(str.lower(_lift_mem_inner(op)))
   if s0.len == 0 { return 0 }
   mut acc = 0
   mut saw_ast = false
   mut i = 0
   mut start = 0
   mut sign = 1
   while i <= s0.len {
      if i == s0.len || load8(s0, i) == 43 || (i > start && load8(s0, i) == 45) {
         def term = slice(s0, start, i, 1)
         def v = _lift_address_term(st, term, dst)
         if _is_ast(v) || saw_ast {
            def sol = state_solver(st)
            def ctx = sol.get("ctx", 0)
            def a = saw_ast ? acc : _lift_value_for_ast(st, dst, acc)
            def b = _lift_value_for_ast(st, dst, v)
            acc = sign < 0 ? smt.bvsub(ctx, a, b) : smt.bvadd(ctx, a, b)
            saw_ast = true
         } else {
            acc = int(acc) + sign * int(v)
         }
         if i < s0.len {
            sign = load8(s0, i) == 45 ? -1 : 1
            start = i + 1
         }
      }
      i += 1
   }
   acc
}

fn _lift_mem_load(dict st, str op) any {
   _lift_mem_load_width(st, op, _lift_operand_bits(st, op) / 8)
}

fn _lift_mem_load_width(dict st, str op, int width=0) any {
   def a = _lift_mem_addr(st, op)
   def w = width <= 0 ? 1 : width
   if w <= 1 {
      def v = mem_read(st, a, 0)
      if is_list(v) && v.len > 0 { return v[0] }
      return v
   }
   mut acc = 0
   mut ast_acc = 0
   mut saw_ast = false
   mut i = 0
   def missing = "__ny_mem_missing__"
   while i < w {
      mut b = mem_read(st, a + i, missing)
      if is_list(b) && b.len > 0 { b = b[0] }
      if is_str(b) && b == missing { return i == 0 ? mem_read(st, a, 0) : (saw_ast ? ast_acc : acc) }
      if _is_ast(b) || saw_ast {
         def sol = state_solver(st)
         def ctx = sol.get("ctx", 0)
         def byte_ast = _is_ast(b) ? b : smt.bv_u8(ctx, int(b) & 255)
         ast_acc = saw_ast ? smt.bvconcat(ctx, byte_ast, ast_acc) :
         (i == 0 ? byte_ast : smt.bvconcat(ctx, byte_ast, smt.bv_u64(ctx, acc, i * 8)))
         saw_ast = true
      } else {
         acc = acc | ((int(b) & 255) << (i * 8))
      }
      i += 1
   }
   if saw_ast { return ast_acc }
   acc
}

fn _lift_mem_width_for_row(dict row, str op) int {
   def bits = _lift_operand_bits(dict(), op)
   if bits == 8 || bits == 16 || bits == 32 || bits == 64 { return bits / 8 }
   def m = str.lower(row.get("mnemonic", ""))
   if m == "sb" || m == "lb" || m == "lbu" || m == "strb" || m == "ldrb" { return 1 }
   if m == "sh" || m == "lh" || m == "lhu" || m == "strh" || m == "ldrh" { return 2 }
   if m == "sw" || m == "lw" || m == "lwu" { return 4 }
   if m == "sd" || m == "ld" { return 8 }
   def dst = str.lower(row.get("dst", ""))
   if str.startswith(dst, "w") { return 4 }
   if str.startswith(dst, "x") { return 8 }
   1
}

fn _lift_mem_store_width(dict st, str op, any value, int width=0) dict {
   def a = _lift_mem_addr(st, op)
   def w = width <= 0 ? 1 : width
   if w <= 1 { return mem_write_byte(st, a, _is_ast(value) ? value : (int(value) & 255)) }
   if _is_ast(value) {
      def ctx = state_solver(st).get("ctx", 0)
      mut out = st
      mut i = 0
      while i < w {
         out = mem_write_byte(out, a + i, smt.bv_extract(ctx, i * 8 + 7, i * 8, value))
         i += 1
      }
      return out
   }
   mut out = st
   mut i = 0
   while i < w {
      out = mem_write_byte(out, a + i, (int(value) >> (i * 8)) & 255)
      i += 1
   }
   out
}

fn _lift_cmp_operands(list rows, int idx) dict {
   if idx < 0 || idx >= rows.len { return dict() }
   def br = rows[idx]
   def args = br.get("args", [])
   def cond = br.get("condition", "")
   if (cond == "zero" || cond == "nonzero") && args.len > 0 { return {"lhs": args[0], "rhs": "0"} }
   if (cond == "bit_zero" || cond == "bit_nonzero") && args.len > 1 { return {"lhs": args[0], "rhs": "0", "bit": args[1]} }
   if br.get("family", "") == "riscv" && args.len > 1 { return {"lhs": args[0], "rhs": args[1]} }
   mut j = idx - 1
   while j >= 0 && idx - j <= 8 {
      def r = rows[j]
      def op = r.get("op", "")
      if op == "call" || op == "branch" || op == "syscall" || op == "return" { break }
      if op == "compare" { return {"lhs": r.get("dst", ""), "rhs": r.get("src", ""), "mnemonic": r.get("mnemonic", "")} }
      if op == "arith" && (cond == "eq" || cond == "ne" || cond == "zero" || cond == "nonzero") {
         return {"lhs": r.get("dst", ""), "rhs": "0"}
      }
      j -= 1
   }
   dict()
}

fn _cmp_text(str lhs, str cond, str rhs) str {
   def op = cond == "eq" || cond == "zero" ? "==" :
   (cond == "ne" || cond == "nonzero" ? "!=" :
      (cond == "gt" || cond == "ugt" ? ">" :
         (cond == "ge" || cond == "uge" ? ">=" :
            (cond == "lt" || cond == "ult" ? "<" :
               (cond == "le" || cond == "ule" ? "<=" : cond)))))
   lhs + " " + op + " " + rhs
}

fn _test_text(str lhs, str cond, str rhs) str {
   def expr = "(" + lhs + " & " + rhs + ")"
   (cond == "ne" || cond == "nonzero") ? (expr + " != 0") : (expr + " == 0")
}

fn _signed_for_bits(any value, int bits) int {
   def b = bits <= 0 ? 64 : bits
   if b >= 63 { return int(value) }
   def mask = b >= 63 ? -1 : ((1 << b) - 1)
   def sign = 1 << (b - 1)
   def v = b >= 63 ? int(value) : (int(value) & mask)
   (v & sign) != 0 ? v - (1 << b) : v
}

fn _lift_is_imm_text(str name) bool {
   def s = str.strip(name)
   if s.len == 0 { return false }
   str.startswith(s, "#") || str.startswith(s, "0x") || str.startswith(s, "-0x") || str.ascii_is_digit(load8(s, 0)) || load8(s, 0) == 45
}

fn _cmp_bool(dict st, str lhs_name, any lhs, str cond, str rhs_name, any rhs) bool {
   def lhs_bits = _lift_operand_bits(st, lhs_name)
   def rhs_bits = _lift_is_imm_text(rhs_name) ? lhs_bits : _lift_operand_bits(st, rhs_name)
   def bits = max(lhs_bits, rhs_bits)
   def ua = int(lhs)
   def ub = int(rhs)
   def sa = _signed_for_bits(lhs, bits)
   def sb = _signed_for_bits(rhs, bits)
   match cond {
      "eq", "zero" -> ua == ub
      "ne", "nonzero" -> ua != ub
      "ugt" -> ua > ub
      "uge" -> ua >= ub
      "ult" -> ua < ub
      "ule" -> ua <= ub
      "gt" -> sa > sb
      "ge" -> sa >= sb
      "lt" -> sa < sb
      "le" -> sa <= sb
      _ -> false
   }
}

fn _test_bool(any lhs, str cond, any rhs) bool {
   def zero = (int(lhs) & int(rhs)) == 0
   (cond == "ne" || cond == "nonzero") ? !zero : zero
}

fn _bit_branch_text(str lhs, str cond, int bit) str {
   def mask = 1 << max(0, bit)
   "(" + lhs + " & 0x" + to_hex(mask, 0) + ")" + (cond == "bit_zero" ? " == 0" : " != 0")
}

fn _bit_branch_bool(any lhs, str cond, int bit) bool {
   def mask = 1 << max(0, bit)
   def zero = (int(lhs) & mask) == 0
   cond == "bit_zero" ? zero : !zero
}

fn _lift_operand_bits(dict st, str name) int {
   def s = str.lower(str.strip(name))
   if str.find(s, "byte ptr") >= 0 { return 8 }
   if str.find(s, "qword ptr") >= 0 { return 64 }
   if str.find(s, "dword ptr") >= 0 { return 32 }
   if str.find(s, "word ptr") >= 0 { return 16 }
   def sy = st.get("symbolics", dict()).get(s, dict())
   if is_dict(sy) && sy.contains("bits") { return int(sy.get("bits", _reg_bits(st, s))) }
   _reg_bits(st, s)
}

fn _bv_const_for(dict st, str reg, any value) any {
   def sol = state_solver(st)
   bvv(sol, int(value), _lift_operand_bits(st, reg))
}

fn _bit_branch_ast(dict st, str lhs_name, any lhs, str cond, int bit) any {
   if !_is_ast(lhs) { return 0 }
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   def bits = _lift_operand_bits(st, lhs_name)
   def mask = bvv(sol, 1 << max(0, bit), bits)
   def zero = bvv(sol, 0, bits)
   def eqz = smt.mk_eq(ctx, smt.bvand(ctx, lhs, mask), zero)
   cond == "bit_zero" ? eqz : smt.mk_not(ctx, eqz)
}

fn _proc_return_zero_constraints(dict st, str reg) list {
   def m = st.get("proc_return_zero_constraints", dict())
   mut out = []
   def aliases = _lift_reg_aliases(reg)
   mut i = 0
   while i < aliases.len {
      def xs = m.get(aliases[i], [])
      if is_list(xs) {
         mut j = 0
         while j < xs.len { out = out.append(xs[j]) j += 1 }
      }
      i += 1
   }
   out
}

fn _proc_return_nonzero_constraints(dict st, str reg) list {
   def m = st.get("proc_return_nonzero_constraints", dict())
   mut out = []
   def aliases = _lift_reg_aliases(reg)
   mut i = 0
   while i < aliases.len {
      def xs = m.get(aliases[i], [])
      if is_list(xs) {
         mut j = 0
         while j < xs.len { out = out.append(xs[j]) j += 1 }
      }
      i += 1
   }
   out
}

fn _proc_return_zero_formula(dict st, str reg, any ret_eq_zero) any {
   def cs = _proc_return_zero_constraints(st, reg)
   if cs.len == 0 { return 0 }
   cs.len == 1 ? cs[0] : smt.mk_and(state_solver(st).get("ctx", 0), cs)
}

fn _proc_return_nonzero_formula(dict st, str reg, any ret_ne_zero) any {
   def cs = _proc_return_nonzero_constraints(st, reg)
   if cs.len == 0 { return 0 }
   cs.len == 1 ? cs[0] : smt.mk_and(state_solver(st).get("ctx", 0), cs)
}

fn _cmp_ast(dict st, str lhs_name, any lhs, str cond, str rhs_name, any rhs) any {
   def breg = _lift_bool_reg_formula(st, lhs_name, cond, rhs)
   if _is_ast(breg) { return breg }
   if (cond == "eq" || cond == "zero" || cond == "ne" || cond == "nonzero") && _lift_operand_is_mem(lhs_name) && _is_ast(rhs) {
      def mem_eq = _mem_value_eq_ast(st, lhs_name, rhs, rhs_name)
      if _is_ast(mem_eq) { return (cond == "ne" || cond == "nonzero") ? smt.mk_not(state_solver(st).get("ctx", 0), mem_eq) : mem_eq }
   }
   if (cond == "eq" || cond == "zero" || cond == "ne" || cond == "nonzero") && _lift_operand_is_mem(rhs_name) && _is_ast(lhs) {
      def mem_eq = _mem_value_eq_ast(st, rhs_name, lhs, lhs_name)
      if _is_ast(mem_eq) { return (cond == "ne" || cond == "nonzero") ? smt.mk_not(state_solver(st).get("ctx", 0), mem_eq) : mem_eq }
   }
   if (cond == "eq" || cond == "zero" || cond == "ne" || cond == "nonzero") && !_is_ast(rhs) && _lift_operand_is_mem(lhs_name) {
      def mem_eq = _mem_imm_eq_ast(st, lhs_name, int(rhs))
      if _is_ast(mem_eq) { return (cond == "ne" || cond == "nonzero") ? smt.mk_not(state_solver(st).get("ctx", 0), mem_eq) : mem_eq }
   }
   if (cond == "eq" || cond == "zero" || cond == "ne" || cond == "nonzero") && !_is_ast(lhs) && _lift_operand_is_mem(rhs_name) {
      def mem_eq = _mem_imm_eq_ast(st, rhs_name, int(lhs))
      if _is_ast(mem_eq) { return (cond == "ne" || cond == "nonzero") ? smt.mk_not(state_solver(st).get("ctx", 0), mem_eq) : mem_eq }
   }
   if !_is_ast(lhs) && !_is_ast(rhs) { return 0 }
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   def l = _is_ast(lhs) ? lhs : _bv_const_for(st, lhs_name, lhs)
   def r = _is_ast(rhs) ? rhs : _bv_const_for(st, lhs_name, rhs)
   if (cond == "eq" || cond == "zero" || cond == "ne" || cond == "nonzero") && !_is_ast(rhs) && int(rhs) == 0 {
      def eqz = smt.mk_eq(ctx, l, r)
      def zero_ret = _proc_return_zero_formula(st, lhs_name, eqz)
      def nonzero_ret = _proc_return_nonzero_formula(st, lhs_name, smt.mk_not(ctx, eqz))
      if cond == "ne" || cond == "nonzero" {
         if _is_ast(nonzero_ret) { return nonzero_ret }
         if _is_ast(zero_ret) { return smt.mk_not(ctx, zero_ret) }
      } else {
         if _is_ast(zero_ret) { return zero_ret }
         if _is_ast(nonzero_ret) { return smt.mk_not(ctx, nonzero_ret) }
      }
   }
   match cond {
      "eq", "zero" -> smt.mk_eq(ctx, l, r)
      "ne", "nonzero" -> smt.mk_not(ctx, smt.mk_eq(ctx, l, r))
      "ugt" -> smt.bvugt(ctx, l, r)
      "uge" -> smt.bvuge(ctx, l, r)
      "ult" -> smt.bvult(ctx, l, r)
      "ule" -> smt.bvule(ctx, l, r)
      "gt" -> smt.bvsgt(ctx, l, r)
      "ge" -> smt.bvsge(ctx, l, r)
      "lt" -> smt.bvslt(ctx, l, r)
      "le" -> smt.bvsle(ctx, l, r)
      _ -> 0
   }
}

fn _lift_operand_is_mem(str op) bool {
   def s = str.lower(str.strip(op))
   str.startswith(s, "mem(") || str.startswith(s, "mem_fs(") || str.startswith(s, "mem_gs(") ||
   str.find(s, "[") >= 0 || _lift_paren_mem_parts(s).len > 0
}

fn _imm_le_bytes(int value, int n) list {
   mut out = []
   mut i = 0
   while i < n {
      mut b = (value >> (i * 8)) & 255
      ;; Ny small ints are signed; qword immediates with bit 62 set arrive
      ;; negative even though the ELF bytes are still unsigned little-endian.
      if value < 0 && n >= 8 && i == n - 1 { b = (b - 128) & 255 }
      out = out.append(b)
      i += 1
   }
   out
}

fn _mem_imm_eq_ast(dict st, str mem_op, int value) any {
   def bits = _lift_operand_bits(st, mem_op)
   def n = max(1, bits / 8)
   def addr = _lift_mem_addr(st, mem_op)
   if addr == 0 || !_mem_has_byte(st, addr) { return 0 }
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   def bytes = _imm_le_bytes(value, n)
   mut cs = []
   mut saw_symbolic = false
   mut i = 0
   while i < bytes.len {
      def actual = _mem_byte_value(st, addr + i)
      def want = int(bytes[i]) & 255
      if _is_ast(actual) {
         saw_symbolic = true
         cs = cs.append(smt.mk_eq(ctx, actual, smt.bv_u8(ctx, want)))
      } elif is_int(actual) {
         if (int(actual) & 255) != want {
            if !saw_symbolic { return 0 }
            return _false_ast(sol)
         }
      } else {
         if !saw_symbolic { return 0 }
         return _false_ast(sol)
      }
      i += 1
   }
   if !saw_symbolic || cs.len == 0 { return 0 }
   cs.len == 1 ? cs[0] : smt.mk_and(ctx, cs)
}

fn _mem_value_eq_ast(dict st, str mem_op, any value, str value_name) any {
   def bits = _lift_operand_bits(st, mem_op)
   def n = max(1, bits / 8)
   def addr = _lift_mem_addr(st, mem_op)
   if addr == 0 || !_mem_has_byte(st, addr) { return 0 }
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   mut cs = []
   mut i = 0
   while i < n {
      def actual = _mem_byte_value(st, addr + i)
      def want = _is_ast(value) ? smt.bv_extract(ctx, i * 8 + 7, i * 8, value) : smt.bv_u8(ctx, (int(value) >> (i * 8)) & 255)
      if _is_ast(actual) {
         cs = cs.append(smt.mk_eq(ctx, actual, want))
      } elif is_int(actual) {
         cs = cs.append(smt.mk_eq(ctx, smt.bv_u8(ctx, int(actual) & 255), want))
      } else {
         return 0
      }
      i += 1
   }
   cs.len == 1 ? cs[0] : smt.mk_and(ctx, cs)
}

fn _test_ast(dict st, str lhs_name, any lhs, str cond, str rhs_name, any rhs) any {
   if !_is_ast(lhs) && !_is_ast(rhs) { return 0 }
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   def l = _is_ast(lhs) ? lhs : _bv_const_for(st, lhs_name, lhs)
   def r = _is_ast(rhs) ? rhs : _bv_const_for(st, lhs_name, rhs)
   def zero = _bv_const_for(st, lhs_name, 0)
   def eqz = smt.mk_eq(ctx, smt.bvand(ctx, l, r), zero)
   if lhs_name == rhs_name && (cond == "eq" || cond == "zero" || cond == "ne" || cond == "nonzero") {
      def zero_ret = _proc_return_zero_formula(st, lhs_name, eqz)
      def nonzero_ret = _proc_return_nonzero_formula(st, lhs_name, smt.mk_not(ctx, eqz))
      if cond == "ne" || cond == "nonzero" {
         if _is_ast(nonzero_ret) { return nonzero_ret }
         if _is_ast(zero_ret) { return smt.mk_not(ctx, zero_ret) }
      } else {
         if _is_ast(zero_ret) { return zero_ret }
         if _is_ast(nonzero_ret) { return smt.mk_not(ctx, nonzero_ret) }
      }
   }
   (cond == "ne" || cond == "nonzero") ? smt.mk_not(ctx, eqz) : eqz
}

fn _lift_bool_reg_formula(dict st, str reg, str cond, any rhs) any {
   def aliases = _lift_reg_aliases(reg)
   def m = st.get("lift_bool_regs", dict())
   mut rec = dict()
   mut i = 0
   while i < aliases.len {
      rec = m.get(aliases[i], dict())
      if is_dict(rec) && rec.contains("ast") { break }
      i += 1
   }
   if !is_dict(rec) || !rec.contains("ast") { return 0 }
   if !is_int(rhs) { return 0 }
   def ast = rec.get("ast", 0)
   if !_is_ast(ast) { return 0 }
   def want_true = int(rhs) != 0
   def ctx = state_solver(st).get("ctx", 0)
   if cond == "eq" || cond == "zero" { return want_true ? ast : smt.mk_not(ctx, ast) }
   if cond == "ne" || cond == "nonzero" { return want_true ? smt.mk_not(ctx, ast) : ast }
   0
}

fn _lifted_branch_state(dict st, dict row, str text, any ast, bool taken, int next) dict {
   mut out = _path_condition(_state_set_pc(state_clone(st), next), text, taken)
   if _is_ast(ast) {
      def sol = state_solver(st)
      def constraint = taken ? ast : smt.mk_not(sol.get("ctx", 0), ast)
      def constraints = _list_copy(out.get("constraints", [])).append(constraint)
      out = out.set("constraints", constraints).set("solver", _solver_with_constraints(sol, constraints))
   }
   _history_append(out.set("step_ok", true).set("step_reason", "ok").set("branch", taken ? "true" : "false"),
      {"addr": row.get("addr", state_addr(st)), "next": next, "backend": "lifted", "condition": text, "branch": taken})
}

fn _lifted_switch_successors(dict st, dict row) list {
   def jt = row.get("jump_table", dict())
   def entries = jt.get("entries", [])
   if entries.len == 0 { return [st.set("step_ok", false).set("step_reason", "empty_jump_table")] }
   def idx_name = jt.get("index_reg", "")
   def idx_value = idx_name.len > 0 ? _lift_get_reg(st, idx_name, nil) : nil
   if is_int(idx_value) {
      def n = int(idx_value)
      if n >= 0 && n < entries.len {
         def target = int(entries[n].get("target", 0))
         return [_lifted_branch_state(st, row, "switch " + idx_name + " == " + to_str(n), 0, true, target)]
      }
      return [st.set("step_ok", false).set("step_reason", "jump_table_index_out_of_range").set("switch_index", n)]
   }
   if _is_ast(idx_value) {
      def sol = state_solver(st)
      def ctx = sol.get("ctx", 0)
      mut out = []
      mut i = 0
      while i < entries.len {
         def target = int(entries[i].get("target", 0))
         def ast = smt.mk_eq(ctx, idx_value, bvv(sol, i, _reg_bits(st, idx_name)))
         out = out.append(_lifted_branch_state(st, row, "switch " + idx_name + " == " + to_str(i), ast, true, target))
         i += 1
      }
      return out
   }
   mut out = []
   mut i = 0
   while i < entries.len {
      out = out.append(_lifted_branch_state(st, row, "switch match " + to_str(i), 0, true, int(entries[i].get("target", 0))))
      i += 1
   }
   out
}

fn state_lifted_successors(dict st, list rows, int idx) list {
   "Fork or step a state using a lifted decompiler branch row.
   This is the ISA-neutral bridge between `lib.os.decomp.lift(...)` rows and
   symbolic path exploration."
   if idx < 0 || idx >= rows.len { return [st.set("step_ok", false).set("step_reason", "bad_lift_index")] }
   def row = rows[idx]
   if row.get("op", "") != "branch" { return [_state_set_pc(st, int(row.get("addr", state_addr(st))) + int(row.get("size", 1)))] }
   def pc = int(row.get("addr", state_addr(st)))
   def target = int(row.get("target", 0))
   def fall = pc + int(row.get("size", 1))
   def cond = row.get("condition", "")
   if row.get("kind", "") == "switch" || row.get("jump_table", dict()).len > 0 { return _lifted_switch_successors(st, row) }
   if cond == "always" { return [_lifted_branch_state(st, row, "always", 0, true, target)] }
   def cmp = _lift_cmp_operands(rows, idx)
   if cmp.len == 0 {
      return [_lifted_branch_state(st, row, cond, 0, true, target), _lifted_branch_state(st, row, cond, 0, false, fall)]
   }
   def lhsn = cmp.get("lhs", "")
   def rhsn = cmp.get("rhs", "0")
   if cond == "bit_zero" || cond == "bit_nonzero" {
      def bit = _parse_lift_imm(to_str(cmp.get("bit", "0")))
      def lhs = _lift_operand_value(st, lhsn)
      def text = _bit_branch_text(lhsn, cond, bit)
      def ast = _bit_branch_ast(st, lhsn, lhs, cond, bit)
      if !_is_ast(ast) {
         def taken = _bit_branch_bool(lhs, cond, bit)
         return [taken ? _lifted_branch_state(st, row, text, 0, true, target) : _lifted_branch_state(st, row, text, 0, false, fall)]
      }
      return [_lifted_branch_state(st, row, text, ast, true, target), _lifted_branch_state(st, row, text, ast, false, fall)]
   }
   def norm = cond == "zero" ? "eq" : (cond == "nonzero" ? "ne" : cond)
   def lhs = _lift_operand_value(st, lhsn)
   def rhs = _lift_operand_value(st, rhsn)
   if str.startswith(str.lower(cmp.get("mnemonic", "")), "test") && (norm == "eq" || norm == "ne") {
      def text = _test_text(lhsn, norm, rhsn)
      def ast = _test_ast(st, lhsn, lhs, norm, rhsn, rhs)
      if !_is_ast(ast) {
         def taken = _test_bool(lhs, norm, rhs)
         return [taken ? _lifted_branch_state(st, row, text, 0, true, target) : _lifted_branch_state(st, row, text, 0, false, fall)]
      }
      return [_lifted_branch_state(st, row, text, ast, true, target), _lifted_branch_state(st, row, text, ast, false, fall)]
   }
   def text = _cmp_text(lhsn, norm, rhsn)
   def ast = _cmp_ast(st, lhsn, lhs, norm, rhsn, rhs)
   if !_is_ast(ast) {
      def taken = _cmp_bool(st, lhsn, lhs, norm, rhsn, rhs)
      return [taken ? _lifted_branch_state(st, row, text, 0, true, target) : _lifted_branch_state(st, row, text, 0, false, fall)]
   }
   [_lifted_branch_state(st, row, text, ast, true, target), _lifted_branch_state(st, row, text, ast, false, fall)]
}

fn _lifted_row_index(list rows, int addr) int {
   mut i = 0
   while i < rows.len {
      if int(rows[i].get("addr", -1)) == addr { return i }
      i += 1
   }
   -1
}

fn _lift_row_context(dict st, dict row) dict {
   if row.get("family", "") == "x86" && int(row.get("addr", 0)) != 0 {
      return _lift_set_reg(st, "rip", int(row.get("addr", state_addr(st))) + int(row.get("size", 0)))
   }
   st
}

fn _x86_alias_info(str reg) dict {
   def r = str.lower(str.strip(reg))
   if r == "rax" || r == "eax" || r == "ax" || r == "al" { return {"full": "rax", "d32": "eax", "w16": "ax", "b8": "al"} }
   if r == "rbx" || r == "ebx" || r == "bx" || r == "bl" { return {"full": "rbx", "d32": "ebx", "w16": "bx", "b8": "bl"} }
   if r == "rcx" || r == "ecx" || r == "cx" || r == "cl" { return {"full": "rcx", "d32": "ecx", "w16": "cx", "b8": "cl"} }
   if r == "rdx" || r == "edx" || r == "dx" || r == "dl" { return {"full": "rdx", "d32": "edx", "w16": "dx", "b8": "dl"} }
   if r == "rsi" || r == "esi" || r == "si" || r == "sil" { return {"full": "rsi", "d32": "esi", "w16": "si", "b8": "sil"} }
   if r == "rdi" || r == "edi" || r == "di" || r == "dil" { return {"full": "rdi", "d32": "edi", "w16": "di", "b8": "dil"} }
   if r == "rbp" || r == "ebp" || r == "bp" || r == "bpl" { return {"full": "rbp", "d32": "ebp", "w16": "bp", "b8": "bpl"} }
   if r == "rsp" || r == "esp" || r == "sp" || r == "spl" { return {"full": "rsp", "d32": "esp", "w16": "sp", "b8": "spl"} }
   dict()
}

fn _int_mask_bits(int bits) int {
   bits >= 63 ? -1 : ((1 << bits) - 1)
}

fn _bv_to_bits(dict st, any value, int from_bits, int to_bits) any {
   if !_is_ast(value) { return int(value) & _int_mask_bits(to_bits) }
   if from_bits == to_bits { return value }
   def ctx = state_solver(st).get("ctx", 0)
   if from_bits < to_bits { return smt.bvzext(ctx, value, to_bits - from_bits) }
   smt.bv_extract(ctx, to_bits - 1, 0, value)
}

fn _x86_set_group_from_full(dict st, dict info, any full_value) dict {
   mut out = st
   if _is_ast(full_value) {
      def ctx = state_solver(st).get("ctx", 0)
      out = state_set_reg(out, info.get("full", ""), full_value)
      out = state_set_reg(out, info.get("d32", ""), smt.bv_extract(ctx, 31, 0, full_value))
      out = state_set_reg(out, info.get("w16", ""), smt.bv_extract(ctx, 15, 0, full_value))
      return state_set_reg(out, info.get("b8", ""), smt.bv_extract(ctx, 7, 0, full_value))
   }
   def v = int(full_value)
   out = state_set_reg(out, info.get("full", ""), v)
   out = state_set_reg(out, info.get("d32", ""), v & 0xffffffff)
   out = state_set_reg(out, info.get("w16", ""), v & 0xffff)
   state_set_reg(out, info.get("b8", ""), v & 0xff)
}

fn _x86_merge_partial(dict st, str full, any old_full, any value, int bits) any {
   if !_is_ast(old_full) && !_is_ast(value) {
      def mask = _int_mask_bits(bits)
      return (int(old_full) & ~mask) | (int(value) & mask)
   }
   def ctx = state_solver(st).get("ctx", 0)
   def old64 = _is_ast(old_full) ? _bv_to_bits(st, old_full, 64, 64) : smt.bv_u64(ctx, int(old_full), 64)
   def val64 = _is_ast(value) ? _bv_to_bits(st, value, bits, 64) : smt.bv_u64(ctx, int(value), 64)
   def low = smt.bv_u64(ctx, _int_mask_bits(bits), 64)
   smt.bvor(ctx, smt.bvand(ctx, old64, smt.bvnot(ctx, low)), smt.bvand(ctx, val64, low))
}

fn _lift_set_reg(dict st, str reg, any value) dict {
   def r = str.lower(str.strip(reg))
   def xi = _x86_alias_info(r)
   if xi.len > 0 {
      if r == xi.get("full", "") { return _x86_set_group_from_full(st, xi, _bv_to_bits(st, value, _reg_bits(st, r), 64)) }
      if r == xi.get("d32", "") { return _x86_set_group_from_full(st, xi, _bv_to_bits(st, value, 32, 64)) }
      def old_full = _lift_get_reg(st, xi.get("full", ""), 0)
      if r == xi.get("w16", "") { return _x86_set_group_from_full(st, xi, _x86_merge_partial(st, xi.get("full", ""), old_full, value, 16)) }
      if r == xi.get("b8", "") { return _x86_set_group_from_full(st, xi, _x86_merge_partial(st, xi.get("full", ""), old_full, value, 8)) }
   }
   mut out = st
   def aliases = _lift_reg_aliases(r)
   mut i = 0
   while i < aliases.len {
      out = state_set_reg(out, aliases[i], value)
      i += 1
   }
   if str.startswith(r, "w") && r.len > 1 { out = state_set_reg(out, "x" + slice(r, 1, r.len, 1), value) }
   elif str.startswith(r, "x") && r.len > 1 { out = state_set_reg(out, "w" + slice(r, 1, r.len, 1), value) }
   out
}

fn _lift_value_for_ast(dict st, str reg, any value) any {
   _is_ast(value) ? value : _bv_const_for(st, reg, value)
}

fn _lift_binary_value(dict st, str dst, any lhs, str op, any rhs) any {
   def bits = _lift_operand_bits(st, dst)
   if _is_ast(lhs) || _is_ast(rhs) {
      def sol = state_solver(st)
      def ctx = sol.get("ctx", 0)
      def l = _lift_value_for_ast(st, dst, lhs)
      def r = _lift_value_for_ast(st, dst, rhs)
      match op {
         "+" -> smt.bvadd(ctx, l, r)
         "-" -> smt.bvsub(ctx, l, r)
         "*" -> smt.bvmul(ctx, l, r)
         "^" -> smt.bvxor(ctx, l, r)
         "&" -> smt.bvand(ctx, l, r)
         "|" -> smt.bvor(ctx, l, r)
         "<<" -> smt.bvshl(ctx, l, r)
         ">>" -> smt.bvlshr(ctx, l, r)
         "rol" -> smt.bvrotl(ctx, l, int(rhs), bits)
         "ror" -> smt.bvrotr(ctx, l, int(rhs), bits)
         _ -> lhs
      }
   } else {
      def a = int(lhs)
      def b = int(rhs)
      def sh = bits > 0 ? (b % bits) : 0
      def mask = bits >= 63 ? -1 : ((1 << bits) - 1)
      def av = a & mask
      match op {
         "+" -> a + b
         "-" -> a - b
         "*" -> a * b
         "^" -> a ^^ b
         "&" -> a & b
         "|" -> a | b
         "<<" -> a << b
         ">>" -> a >> b
         "rol" -> sh == 0 ? av : (((av << sh) | (av >> (bits - sh))) & mask)
         "ror" -> sh == 0 ? av : (((av >> sh) | (av << (bits - sh))) & mask)
         _ -> lhs
      }
   }
}

fn _lift_unary_value(dict st, str dst, str op, any value) any {
   def bits = _lift_operand_bits(st, dst)
   if _is_ast(value) {
      def ctx = state_solver(st).get("ctx", 0)
      if op == "~" { return smt.bvnot(ctx, value) }
      if op == "neg" { return smt.bvneg(ctx, value) }
      return value
   }
   def mask = bits >= 63 ? -1 : ((1 << bits) - 1)
   def v = int(value) & mask
   if op == "~" { return (~v) & mask }
   if op == "neg" { return (-v) & mask }
   value
}

fn _lift_signed32(any value) int {
   def v = int(value) & 0xffffffff
   v >= 0x80000000 ? v - 0x100000000 : v
}

fn _lift_apply_x86_div(dict st, dict row) dict {
   def rhs = _lift_operand_value(st, row.get("src", row.get("dst", "")))
   if _is_ast(rhs) { return st }
   def den = int(rhs)
   if den == 0 { return st }
   def eax0 = _lift_get_reg(st, "eax", 0)
   if _is_ast(eax0) { return st }
   def num = row.get("kind", "") == "signed_div" ? _lift_signed32(eax0) : (int(eax0) & 0xffffffff)
   def quot = num / den
   def rem = num % den
   _lift_set_reg(_lift_set_reg(st, "eax", quot), "edx", rem)
}

fn _lift_setcc(dict st, dict row) dict {
   def dst = row.get("dst", "")
   def ce = _lift_row_condition_eval(st, row)
   if dst.len == 0 || !ce.get("ok", false) { return st }
   if _is_ast(ce.get("ast", 0)) {
      mut out = _lift_set_reg(st, dst, 0)
      mut m = out.get("lift_bool_regs", dict())
      def aliases = _lift_reg_aliases(dst)
      mut i = 0
      while i < aliases.len {
         m = m.set(aliases[i], {"ast": ce.get("ast", 0), "condition": ce.get("condition", ""), "lhs": ce.get("lhs", ""), "rhs": ce.get("rhs", "")})
         i += 1
      }
      return out.set("lift_bool_regs", m)
   }
   _lift_set_reg(st, dst, ce.get("taken", false) ? 1 : 0)
}

fn _lift_row_condition_eval(dict st, dict row) dict {
   def cond = row.get("condition", "")
   def lhsn = row.get("cmp_lhs", "")
   def rhsn = row.get("cmp_rhs", "")
   if cond.len == 0 || lhsn.len == 0 { return {"ok": false} }
   def lhs = _lift_operand_value(st, lhsn)
   def rhs = _lift_operand_value(st, rhsn)
   if str.startswith(str.lower(row.get("cmp_mnemonic", "")), "test") && (cond == "eq" || cond == "ne") {
      def ast = _test_ast(st, lhsn, lhs, cond, rhsn, rhs)
      if _is_ast(ast) { return {"ok": true, "condition": cond, "lhs": lhsn, "rhs": rhsn, "ast": ast} }
      return {"ok": true, "condition": cond, "lhs": lhsn, "rhs": rhsn, "taken": _test_bool(lhs, cond, rhs)}
   }
   def ast = _cmp_ast(st, lhsn, lhs, cond, rhsn, rhs)
   if _is_ast(ast) { return {"ok": true, "condition": cond, "lhs": lhsn, "rhs": rhsn, "ast": ast} }
   {"ok": true, "condition": cond, "lhs": lhsn, "rhs": rhsn, "taken": _cmp_bool(st, lhsn, lhs, cond, rhsn, rhs)}
}

fn _lift_conditioned_linear_state(dict st, dict row, any ast, bool taken, int next) dict {
   def text = row.get("dst", "") + " = " + row.get("condition", "")
   def base = state_clone(st).set("regs", _dict_copy(state_regs(st))).set("mem", _dict_copy(state_mem(st)))
   mut out = _path_condition(_state_set_pc(base, next), text, taken)
   if _is_ast(ast) {
      def sol = state_solver(st)
      def constraint = taken ? ast : smt.mk_not(sol.get("ctx", 0), ast)
      def constraints = _list_copy(out.get("constraints", [])).append(constraint)
      out = out.set("constraints", constraints).set("solver", _solver_with_constraints(sol, constraints))
   }
   _history_append(out.set("step_ok", true).set("step_reason", "ok").set("branch", taken ? "true" : "false"),
      {"addr": row.get("addr", state_addr(st)), "next": next, "backend": "lifted", "condition": text, "branch": taken, "insn": row.get("mnemonic", "cmov")})
}

fn _lift_cmov_successors(dict st, dict row, int next) list {
   def dst = row.get("dst", "")
   if dst.len == 0 { return [_state_set_pc(st, next)] }
   def ce = _lift_row_condition_eval(st, row)
   if !ce.get("ok", false) { return [_state_set_pc(st, next)] }
   if _is_ast(ce.get("ast", 0)) {
      def moved = _lift_set_reg(_lift_conditioned_linear_state(st, row, ce.get("ast", 0), true, next), dst, _lift_operand_value(st, row.get("src", "")))
      def kept = _lift_conditioned_linear_state(st, row, ce.get("ast", 0), false, next)
      return [moved, kept]
   }
   if ce.get("taken", false) { return [_state_set_pc(_lift_set_reg(st, dst, _lift_operand_value(st, row.get("src", ""))), next)] }
   [_state_set_pc(st, next)]
}

fn _lift_extend_load_value(dict st, dict row, any value) any {
   def dst = row.get("dst", "")
   def src_bits = max(1, _lift_mem_width_for_row(row, row.get("src", "")) * 8)
   def dst_bits = max(src_bits, _lift_operand_bits(st, dst))
   if dst_bits <= src_bits { return value }
   def extra = dst_bits - src_bits
   def is_signed = str.startswith(str.lower(row.get("mnemonic", "")), "movsx")
   if _is_ast(value) {
      def ctx = state_solver(st).get("ctx", 0)
      return is_signed ? smt.bvsext(ctx, value, extra) : smt.bvzext(ctx, value, extra)
   }
   def mask = (1 << src_bits) - 1
   def v = int(value) & mask
   if is_signed && (v & (1 << (src_bits - 1))) != 0 {
      return v | (((1 << extra) - 1) << src_bits)
   }
   v
}

fn _lift_sign_extend_value(dict st, any value, int from_bits, int to_bits) any {
   def fb = max(1, from_bits)
   def tb = max(fb, to_bits)
   if tb <= fb { return value }
   if _is_ast(value) { return smt.bvsext(state_solver(st).get("ctx", 0), value, tb - fb) }
   def mask = _int_mask_bits(fb)
   def v = int(value) & mask
   if (v & (1 << (fb - 1))) != 0 { return v | (-1 << fb) }
   v
}

fn _x86_low8_alias(str reg) str {
   def r = str.lower(str.strip(reg))
   match r {
      "rax", "eax", "ax", "al" -> "al"
      "rbx", "ebx", "bx", "bl" -> "bl"
      "rcx", "ecx", "cx", "cl" -> "cl"
      "rdx", "edx", "dx", "dl" -> "dl"
      "rsi", "esi", "si", "sil" -> "sil"
      "rdi", "edi", "di", "dil" -> "dil"
      "rbp", "ebp", "bp", "bpl" -> "bpl"
      "rsp", "esp", "sp", "spl" -> "spl"
      _ -> ""
   }
}

fn _lift_set_reg_preserve_low8(dict st, str dst, any extended, any raw, int src_bits) dict {
   mut out = _lift_set_reg(st, dst, extended)
   if src_bits != 8 { return out }
   def low = _x86_low8_alias(dst)
   if low.len == 0 { return out }
   state_set_reg(out, low, raw)
}

fn _apply_lifted_stack_effect(dict st, dict row) dict {
   def m = row.get("mnemonic", "")
   def dst = row.get("dst", "")
   if m == "push" {
      def sp = int(_lift_get_reg(st, "rsp", 0)) - 8
      def out = _lift_set_reg(st, "rsp", sp)
      return mem_write(out, sp, _lift_operand_value(st, dst))
   }
   if m == "pop" {
      def sp = int(_lift_get_reg(st, "rsp", 0))
      def out = _lift_set_reg(st, dst, mem_read(st, sp, 0))
      return _lift_set_reg(out, "rsp", sp + 8)
   }
   if m == "leave" {
      def bp = int(_lift_get_reg(st, "rbp", 0))
      mut out = _lift_set_reg(st, "rsp", bp)
      out = _lift_set_reg(out, "rbp", mem_read(out, bp, 0))
      return _lift_set_reg(out, "rsp", bp + 8)
   }
   st
}

fn _apply_lifted_row_effect(dict st, dict row) dict {
   def cur = _lift_row_context(st, row)
   def op = row.get("op", "")
   def dst = row.get("dst", "")
   if op == "stack" { return _apply_lifted_stack_effect(cur, row) }
   if row.get("mnemonic", "") == "cdqe" { return _lift_set_reg(cur, "rax", _lift_sign_extend_value(cur, _lift_get_reg(cur, "eax", 0), 32, 64)) }
   if row.get("mnemonic", "") == "pxor" || row.get("mnemonic", "") == "xorps" || row.get("mnemonic", "") == "xorpd" {
      return _lift_set_reg(cur, dst, _lift_binary_value(cur, dst, _lift_get_reg(cur, dst, 0), "^", _lift_operand_value(cur, row.get("src", ""))))
   }
   if op == "assign" && row.get("dst_kind", "") == "mem" {
      return _lift_mem_store_width(cur, dst, _lift_operand_value(cur, row.get("src", "")), _lift_mem_width_for_row(row, dst))
   }
   if op == "arith" && row.get("dst_kind", "") == "mem" {
      def kind = row.get("kind", "")
      if kind == "unary" { return cur }
      def oper = row.get("operator", "")
      if oper == "+" || oper == "-" || oper == "*" || oper == "^" || oper == "&" || oper == "|" || oper == "<<" || oper == ">>" {
         def w = _lift_mem_width_for_row(row, dst)
         def lhs = _lift_mem_load_width(cur, dst, w)
         def rhs = row.get("src_kind", "") == "none" ? 1 : _lift_operand_value(cur, row.get("src", "1"))
         return _lift_mem_store_width(cur, dst, _lift_binary_value(cur, dst, lhs, oper, rhs), w)
      }
      return cur
   }
   if dst.len == 0 || row.get("dst_kind", "") != "reg" { return cur }
   if op == "assign" {
      if row.get("kind", "") == "setcc" { return _lift_setcc(cur, row) }
      if row.get("kind", "") == "cmovcc" {
         def ce = _lift_row_condition_eval(cur, row)
         if ce.get("ok", false) && ce.get("taken", false) { return _lift_set_reg(cur, dst, _lift_operand_value(cur, row.get("src", ""))) }
         return cur
      }
      if row.get("kind", "") == "lea" {
         def src = row.get("src", "")
         return _lift_set_reg(cur, dst, _lift_address_value(cur, src, "rax"))
      }
      if row.get("kind", "") == "swap" && row.get("src_kind", "") == "reg" {
         def src = row.get("src", "")
         def a = _lift_get_reg(cur, dst, 0)
         def b = _lift_get_reg(cur, src, 0)
         return _lift_set_reg(_lift_set_reg(cur, dst, b), src, a)
      }
      if row.get("kind", "") == "byte_load" && row.get("src_kind", "") == "mem" {
         def raw = _lift_mem_load_width(cur, row.get("src", ""), _lift_mem_width_for_row(row, row.get("src", "")))
         def src_bits = max(1, _lift_mem_width_for_row(row, row.get("src", "")) * 8)
         return _lift_set_reg_preserve_low8(cur, dst, _lift_extend_load_value(cur, row, raw), raw, src_bits)
      }
      if row.get("kind", "") == "byte_load" && row.get("src_kind", "") == "reg" {
         def raw = _lift_operand_value(cur, row.get("src", ""))
         def src_bits = max(1, _lift_mem_width_for_row(row, row.get("src", "")) * 8)
         return _lift_set_reg_preserve_low8(cur, dst, _lift_extend_load_value(cur, row, raw), raw, src_bits)
      }
      if row.get("src_kind", "") == "mem" { return _lift_set_reg(cur, dst, _lift_mem_load_width(cur, row.get("src", ""), _lift_mem_width_for_row(row, row.get("src", "")))) }
      if row.get("src_kind", "") == "reg" || row.get("src_kind", "") == "imm" {
         return _lift_set_reg(cur, dst, _lift_operand_value(cur, row.get("src", "")))
      }
      return cur
   }
   if op == "arith" {
      def kind = row.get("kind", "")
      if kind == "sign_extend_dividend" {
         def v = _lift_get_reg(cur, row.get("src", "eax"), 0)
         if _is_ast(v) { return cur }
         return _lift_set_reg(cur, row.get("dst", "edx"), (int(v) & 0x80000000) != 0 ? -1 : 0)
      }
      if kind == "signed_div" || kind == "unsigned_div" { return _lift_apply_x86_div(cur, row) }
      if kind == "zeroing_idiom" { return _lift_set_reg(cur, dst, 0) }
      if kind == "imm_load" && row.get("args", []).len > 2 { return _lift_set_reg(cur, dst, _lift_operand_value(cur, row.get("args", [])[2])) }
      if kind == "unary" {
         return _lift_set_reg(cur, dst, _lift_unary_value(cur, dst, row.get("operator", ""), _lift_get_reg(cur, dst, 0)))
      }
      if kind == "mul3" {
         return _lift_set_reg(cur, dst, _lift_binary_value(cur, dst, _lift_operand_value(cur, row.get("src", "")), "*", _lift_operand_value(cur, row.get("src2", "1"))))
      }
      def oper = row.get("operator", "")
      if oper == "+" || oper == "-" || oper == "*" || oper == "^" || oper == "&" || oper == "|" || oper == "<<" || oper == ">>" || oper == "rol" || oper == "ror" {
         def lhs = _lift_get_reg(cur, dst, 0)
         def rhs = row.get("src_kind", "") == "none" ? 1 : _lift_operand_value(cur, row.get("src", "1"))
         return _lift_set_reg(cur, dst, _lift_binary_value(cur, dst, lhs, oper, rhs))
      }
   }
   cur
}

fn _lifted_step_state(dict st, list rows, any opts=dict()) list {
   def idx = _lifted_row_index(rows, state_addr(st))
   if idx < 0 { return [st.set("step_ok", false).set("step_reason", "lifted_pc_not_found")] }
   def row = rows[idx]
   def op = row.get("op", "")
   if op == "branch" { return state_lifted_successors(st, rows, idx) }
   if op == "return" {
      def popped = _call_pop(st)
      if popped.get("ok", false) {
         return [_history_append(_state_set_pc(popped.get("state", st), int(popped.get("ret", state_addr(st)))).set("step_ok", true).set("step_reason", "ok"),
               {"addr": state_addr(st), "next": int(popped.get("ret", state_addr(st))), "backend": "lifted", "insn": "return"})]
      }
      return [_history_append(st.set("deadended", true).set("step_ok", true).set("step_reason", "ok"),
            {"addr": state_addr(st), "next": state_addr(st), "backend": "lifted", "insn": "return"})]
   }
   mut next = int(row.get("addr", state_addr(st))) + int(row.get("size", 1))
   if op == "call" {
      def target = int(row.get("target", 0))
      if target > 0 {
         def hook = _project_hook_at(st.get("project", 0), target)
         if hook.len > 0 { return [_apply_hook(st, hook.set("ret", next))] }
         if opts.get("follow_calls", false) {
            def out_call = _call_push(st, next)
            return [_history_append(_state_set_pc(out_call, target).set("step_ok", true).set("step_reason", "ok"),
                  {"addr": row.get("addr", state_addr(st)), "next": target, "backend": "lifted", "insn": "call", "call_return": next})]
         }
      }
   }
   if row.get("kind", "") == "cmovcc" { return _lift_cmov_successors(st, row, next) }
   if op == "syscall" {
      def out_sys = _apply_linux_syscall(st)
      return [_history_append(_state_set_pc(out_sys, next).set("step_ok", true).set("step_reason", "ok"),
            {"addr": row.get("addr", state_addr(st)), "next": next, "backend": "lifted", "insn": row.get("mnemonic", "syscall"),
               "syscall": out_sys.get("last_syscall", -1), "syscall_name": out_sys.get("last_syscall_name", "")})]
   }
   def out = _apply_lifted_row_effect(st, row)
   [_history_append(_state_set_pc(out, next).set("step_ok", true).set("step_reason", "ok"),
         {"addr": row.get("addr", state_addr(st)), "next": next, "backend": "lifted", "insn": row.get("mnemonic", op)})]
}

fn _read_cstring(dict st, int addr, int max_len=4096) str {
   if addr <= 0 || max_len <= 0 { return "" }
   mut out = str.Builder(32)
   mut i = 0
   while i < max_len {
      def b = mem_read(st, addr + i, -1)
      if !is_int(b) || int(b) <= 0 { break }
      out = str.builder_append_byte(out, int(b) & 255)
      i += 1
   }
   def s = str.builder_to_str(out)
   str.builder_free(out)
   s
}

fn _proc_cmp(str a, str b, int n=-1) int {
   def lim = n < 0 ? max(a.len, b.len) : n
   mut i = 0
   while i < lim {
      def av = i < a.len ? (load8(a, i) & 255) : 0
      def bv = i < b.len ? (load8(b, i) & 255) : 0
      if av != bv { return av < bv ? -1 : 1 }
      i += 1
   }
   0
}

fn _ascii_lower_str(str s) str {
   mut b = str.Builder(s.len)
   mut i = 0
   while i < s.len {
      def c = load8(s, i) & 255
      b = str.builder_append_byte(b, c >= 65 && c <= 90 ? c + 32 : c)
      i += 1
   }
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _proc_memcmp(dict st, int a, int b, int n) int {
   mut i = 0
   while i < n {
      def av = mem_read(st, a + i, -1)
      def bv = mem_read(st, b + i, -1)
      if !is_int(av) || !is_int(bv) { return 1 }
      if int(av) != int(bv) { return (int(av) & 255) < (int(bv) & 255) ? -1 : 1 }
      i += 1
   }
   0
}

fn _mem_byte_value(dict st, int addr) any {
   def v = mem_read(st, addr, nil)
   if is_list(v) && v.len > 0 { return v[0] }
   v
}

fn _false_ast(dict sol) any {
   def ctx = sol.get("ctx", 0)
   smt.mk_eq(ctx, smt.bv_u8(ctx, 0), smt.bv_u8(ctx, 1))
}

fn _concrete_mem_bytes(dict st, int addr, int n) dict {
   if addr <= 0 || n < 0 { return {"ok": false, "bytes": []} }
   mut out = []
   mut i = 0
   while i < n {
      def b = _mem_byte_value(st, addr + i)
      if !is_int(b) { return {"ok": false, "bytes": out} }
      out = out.append(int(b) & 255)
      i += 1
   }
   {"ok": true, "bytes": out}
}

fn _concrete_cstring_bytes(dict st, int addr, int max_len) dict {
   if addr <= 0 || max_len <= 0 { return {"ok": false, "bytes": []} }
   mut out = []
   mut i = 0
   while i < max_len {
      def b = _mem_byte_value(st, addr + i)
      if !is_int(b) { return {"ok": false, "bytes": out} }
      def v = int(b) & 255
      out = out.append(v)
      if v == 0 { return {"ok": true, "bytes": out} }
      i += 1
   }
   {"ok": false, "bytes": out}
}

fn _mem_eq_constraints(dict st, int addr, list bytes) list {
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   mut out = []
   mut i = 0
   while i < bytes.len {
      def actual = _mem_byte_value(st, addr + i)
      def want = int(bytes[i]) & 255
      if _is_ast(actual) {
         out = out.append(smt.mk_eq(ctx, actual, smt.bv_u8(ctx, want)))
      } elif is_int(actual) {
         if (int(actual) & 255) != want { out = out.append(_false_ast(sol)) }
      } else {
         out = out.append(_false_ast(sol))
      }
      i += 1
   }
   out
}

fn _mem_pair_eq_constraints(dict st, int a, int b, int n) list {
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   mut out = []
   mut i = 0
   while i < n {
      def av = _mem_byte_value(st, a + i)
      def bv = _mem_byte_value(st, b + i)
      if _is_ast(av) || _is_ast(bv) {
         if (_is_ast(av) || is_int(av)) && (_is_ast(bv) || is_int(bv)) {
            def lhs = _is_ast(av) ? av : smt.bv_u8(ctx, int(av) & 255)
            def rhs = _is_ast(bv) ? bv : smt.bv_u8(ctx, int(bv) & 255)
            out = out.append(smt.mk_eq(ctx, lhs, rhs))
         } else {
            out = out.append(_false_ast(sol))
         }
      } elif is_int(av) && is_int(bv) {
         if (int(av) & 255) != (int(bv) & 255) { out = out.append(_false_ast(sol)) }
      } else {
         out = out.append(_false_ast(sol))
      }
      i += 1
   }
   out
}

fn _proc_symbolic_return(dict st, str proc) dict {
   def sol = state_solver(st)
   def name = "proc_" + proc + "_" + to_str(st.get("history", []).len)
   def ret = _abi_return_reg(st)
   def ast = bvs(sol, name, _reg_bits(st, ret))
   _abi_set_return(st.set("solver", sol), ast)
}

fn _proc_symbolic_return_zero(dict st, str proc, list constraints) dict {
   mut out = _proc_symbolic_return(st, proc)
   if constraints.len == 0 { return out }
   def ret = _abi_return_reg(out)
   def aliases = _lift_reg_aliases(ret)
   mut m = out.get("proc_return_zero_constraints", dict())
   mut i = 0
   while i < aliases.len {
      m = m.set(aliases[i], constraints)
      i += 1
   }
   out.set("proc_return_zero_constraints", m)
}

fn _not_all_constraints(dict st, list constraints) list {
   mut cs = []
   mut i = 0
   while i < constraints.len {
      if _is_ast(constraints[i]) { cs = cs.append(constraints[i]) }
      i += 1
   }
   if cs.len == 0 { return [] }
   def ctx = state_solver(st).get("ctx", 0)
   [smt.mk_not(ctx, cs.len == 1 ? cs[0] : smt.mk_and(ctx, cs))]
}

fn _proc_symbolic_return_eqness(dict st, str proc, list zero_constraints) dict {
   _proc_symbolic_return_nullness(st, proc, zero_constraints, _not_all_constraints(st, zero_constraints))
}

fn _proc_symbolic_return_nullness(dict st, str proc, list zero_constraints, list nonzero_constraints) dict {
   mut out = _proc_symbolic_return(st, proc)
   def ret = _abi_return_reg(out)
   def aliases = _lift_reg_aliases(ret)
   if zero_constraints.len > 0 {
      mut zm = out.get("proc_return_zero_constraints", dict())
      mut zi = 0
      while zi < aliases.len {
         zm = zm.set(aliases[zi], zero_constraints)
         zi += 1
      }
      out = out.set("proc_return_zero_constraints", zm)
   }
   if nonzero_constraints.len > 0 {
      mut nzm = out.get("proc_return_nonzero_constraints", dict())
      mut ni = 0
      while ni < aliases.len {
         nzm = nzm.set(aliases[ni], nonzero_constraints)
         ni += 1
      }
      out = out.set("proc_return_nonzero_constraints", nzm)
   }
   out
}

fn _state_input_len(dict st) int {
   def input = state_stdin(st)
   _data_len(input)
}

fn _state_input_at(dict st, int idx) any {
   def input = state_stdin(st)
   _item_at(input, idx)
}

fn _state_read_stdin(dict st, int dst, int n) dict {
   def pos = int(st.get("stdin_pos", 0))
   def total = _state_input_len(st)
   def count = min(max(0, n), max(0, total - pos))
   mut out = st
   mut i = 0
   while i < count {
      out = mem_write_byte(out, dst + i, _state_input_at(out, pos + i))
      i += 1
   }
   out.set("stdin_pos", pos + count).set("last_io_count", count).set("last_input_dst", dst).set("last_input_count", count)
}

fn _fs_path_variants(str path) list {
   if path.len > 1 && str.find(path, "/") == 0 { return [path, slice(path, 1, path.len, 1)] }
   [path, "/" + path]
}

fn _fs_lookup(dict st, str path) any {
   def fs = state_fs(st)
   if fs.contains(path) { return fs.get(path) }
   def variants = _fs_path_variants(path)
   mut i = 0
   while i < variants.len {
      if fs.contains(variants[i]) { return fs.get(variants[i]) }
      i += 1
   }
   nil
}

fn _fs_exists(dict st, str path) bool {
   _fs_lookup(st, path) != nil
}

fn _fs_data(any rec) any {
   is_dict(rec) ? rec.get("data", rec.get("content", "")) : rec
}

fn _fs_size(dict st, str path) int {
   def rec = _fs_lookup(st, path)
   rec == nil ? 0 : _data_len(_fs_data(rec))
}

fn _fs_mode(any rec) int {
   if is_dict(rec) { return int(rec.get("mode", rec.get("type", "") == "dir" ? 0x41ed : 0x81a4)) }
   0x81a4
}

fn _fs_link_target(dict st, str path) str {
   def rec = _fs_lookup(st, path)
   if is_dict(rec) { return to_str(rec.get("target", rec.get("link", rec.get("readlink", "")))) }
   ""
}

fn _fd_record(dict st, int fd) dict {
   def t = st.get("fd_table", dict())
   def k = to_str(fd)
   is_dict(t.get(k, dict())) ? t.get(k, dict()) : dict()
}

fn _fd_set(dict st, int fd, dict rec) dict {
   st.set("fd_table", st.get("fd_table", dict()).set(to_str(fd), rec))
}

fn _fd_drop(dict st, int fd) dict {
   st.set("fd_table", st.get("fd_table", dict()).set(to_str(fd), dict()))
}

fn _state_dup_fd(dict st, int oldfd, int newfd=-1) dict {
   def rec = _fd_record(st, oldfd)
   if rec.len == 0 { return state_set_reg(st, "rax", -9) }
   mut fd = newfd
   mut out = st
   if fd < 0 {
      fd = max(3, int(out.get("next_fd", 3)))
      out = out.set("next_fd", fd + 1)
   } else {
      out = out.set("next_fd", max(int(out.get("next_fd", 3)), fd + 1))
   }
   state_set_reg(_fd_set(out, fd, rec.set("dup_of", oldfd)).set("last_fd", fd), "rax", fd)
}

fn _state_pipe(dict st, int dst) dict {
   if dst <= 0 { return state_set_reg(st, "rax", -14) }
   def rd = max(3, int(st.get("next_fd", 3)))
   def wr = rd + 1
   mut out = st.set("next_fd", wr + 1)
   out = _fd_set(out, rd, {"kind": "pipe", "data": "", "pos": 0, "peer": wr})
   out = _fd_set(out, wr, {"kind": "pipe", "data": "", "pos": 0, "peer": rd, "sent": ""})
   out = _mem_write_i32(out, dst, rd)
   out = _mem_write_i32(out, dst + 4, wr)
   state_set_reg(out.set("last_pipe_read", rd).set("last_pipe_write", wr), "rax", 0)
}

fn _state_open_file(dict st, str path) dict {
   def data = _fs_lookup(st, path)
   if data == nil { return state_set_reg(st, "rax", -2).set("last_open_path", path) }
   def fd = max(3, int(st.get("next_fd", 3)))
   def rec = {"path": path, "data": _fs_data(data), "mode": _fs_mode(data), "pos": 0}
   _fd_set(st.set("next_fd", fd + 1).set("last_open_path", path), fd, rec).set("last_fd", fd)
}

fn _state_access_path(dict st, str path) dict {
   state_set_reg(st.set("last_access_path", path), "rax", _fs_exists(st, path) ? 0 : -2)
}

fn _state_write_stat(dict st, int dst, int size, int mode) dict {
   if dst <= 0 { return state_set_reg(st, "rax", -14) }
   mut out = st
   out = _mem_write_i64(out, dst, 1)
   out = _mem_write_i64(out, dst + 8, 1)
   out = _mem_write_i64(out, dst + 16, 1)
   out = _mem_write_i32(out, dst + 24, mode)
   out = _mem_write_i64(out, dst + 48, size)
   state_set_reg(out, "rax", 0)
}

fn _state_stat_path(dict st, str path, int dst) dict {
   def rec = _fs_lookup(st, path)
   if rec == nil { return state_set_reg(st.set("last_stat_path", path), "rax", -2) }
   _state_write_stat(st.set("last_stat_path", path), dst, _data_len(_fs_data(rec)), _fs_mode(rec))
}

fn _state_statx_path(dict st, str path, int dst) dict {
   def rec = _fs_lookup(st, path)
   if rec == nil { return state_set_reg(st.set("last_stat_path", path), "rax", -2) }
   if dst <= 0 { return state_set_reg(st.set("last_stat_path", path), "rax", -14) }
   mut out = st.set("last_stat_path", path)
   out = _mem_write_i32(out, dst, 0x17ff)
   out = _mem_write_i32(out, dst + 28, _fs_mode(rec))
   out = _mem_write_i64(out, dst + 40, _data_len(_fs_data(rec)))
   state_set_reg(out, "rax", 0)
}

fn _state_fstat_fd(dict st, int fd, int dst) dict {
   def rec = _fd_record(st, fd)
   if rec.len == 0 { return state_set_reg(st, "rax", -9) }
   _state_write_stat(st, dst, _data_len(rec.get("data", "")), int(rec.get("mode", 0x81a4)))
}

fn _state_readlink_path(dict st, str path, int dst, int n) dict {
   if dst <= 0 || n <= 0 { return state_set_reg(st.set("last_readlink_path", path), "rax", -22) }
   def target = _fs_link_target(st, path)
   if target.len == 0 { return state_set_reg(st.set("last_readlink_path", path), "rax", -22) }
   def count = min(n, target.len)
   mut out = st
   mut i = 0
   while i < count {
      out = mem_write_byte(out, dst + i, load8(target, i))
      i += 1
   }
   state_set_reg(out.set("last_readlink_path", path), "rax", count)
}

fn _state_uname(dict st, int dst) dict {
   if dst <= 0 { return state_set_reg(st, "rax", -14) }
   def proc = st.get("process", dict())
   def vals = [proc.get("sysname", "Linux"), proc.get("nodename", "nytrix"),
      proc.get("release", "6.0.0"), proc.get("version", "Nytrix virtual kernel"),
      proc.get("machine", st.get("arch", dict()).get("arch", "x86_64")),
      proc.get("domainname", "localdomain")]
   mut out = st
   mut i = 0
   while i < vals.len {
      out = _write_cstr(out, dst + i * 65, vals[i])
      i += 1
   }
   state_set_reg(out, "rax", 0)
}

fn _state_read_fd(dict st, int fd, int dst, int n) dict {
   if fd == 0 { return _state_read_stdin(st, dst, n) }
   def rec = _fd_record(st, fd)
   if rec.len == 0 || dst <= 0 || n <= 0 { return st.set("last_io_count", 0) }
   def data = rec.get("data", "")
   def pos = int(rec.get("pos", 0))
   def left = max(0, _data_len(data) - pos)
   def count = min(n, left)
   mut out = st
   mut i = 0
   while i < count {
      out = mem_write_byte(out, dst + i, _item_at(data, pos + i))
      i += 1
   }
   def updated = rec.set("pos", pos + count)
   _fd_set(out, fd, updated).set("last_io_count", count).set("last_input_dst", dst).set("last_input_count", count)
}

fn _state_seek_fd(dict st, int fd, int off, int whence) dict {
   def rec = _fd_record(st, fd)
   if rec.len == 0 { return state_set_reg(st, "rax", -9) }
   def data = rec.get("data", "")
   def base = whence == 1 ? int(rec.get("pos", 0)) : (whence == 2 ? _data_len(data) : 0)
   def pos = max(0, min(_data_len(data), base + off))
   state_set_reg(_fd_set(st, fd, rec.set("pos", pos)), "rax", pos)
}

fn _state_socket(dict st, int domain, int typ, int proto) dict {
   def fd = max(3, int(st.get("next_fd", 3)))
   def net = state_net(st)
   def data = net.get("recv", net.get("rx", ""))
   def rec = {"kind": "socket", "domain": domain, "type": typ, "protocol": proto,
      "data": data, "pos": 0, "sent": "", "connected": false}
   _fd_set(st.set("next_fd", fd + 1), fd, rec).set("last_fd", fd)
}

fn _state_connect_fd(dict st, int fd, int addr, int len) dict {
   def rec = _fd_record(st, fd)
   if rec.len == 0 { return state_set_reg(st, "rax", -9) }
   state_set_reg(_fd_set(st, fd, rec.set("connected", true).set("peer_addr", addr).set("peer_len", len)).set("last_fd", fd), "rax", 0)
}

fn _state_recv_fd(dict st, int fd, int dst, int n) dict {
   _state_read_fd(st, fd, dst, n)
}

fn _state_send_fd(dict st, int fd, int src, int n) dict {
   def rec = _fd_record(st, fd)
   def data = mem_read_bytes(st, src, n)
   if (fd == 1 || fd == 2) && src > 0 && n > 0 {
      def with_out = fd == 1 ? state_append_stdout(st, data) : state_append_stderr(st, data)
      return with_out.set("last_io_count", data.len).set("last_output_src", src).set("last_output_count", data.len)
   }
   if rec.len == 0 || src <= 0 || n <= 0 { return st.set("last_io_count", 0) }
   def updated = rec.set("sent", to_str(rec.get("sent", "")) + data)
   _fd_set(st.set("net_tx", to_str(st.get("net_tx", "")) + data), fd, updated).set("last_io_count", data.len).set("last_output_src", src).set("last_output_count", data.len)
}

fn _state_readv_fd(dict st, int fd, int iov, int count) dict {
   mut out = st
   mut total = 0
   mut i = 0
   while i < count {
      def base = _mem_read_ptr(out, iov + i * 16, 0)
      def n = _mem_read_ptr(out, iov + i * 16 + 8, 0)
      if base <= 0 || n <= 0 { break }
      out = _state_read_fd(out, fd, base, n)
      def got = int(out.get("last_io_count", 0))
      total += got
      if got < n { break }
      i += 1
   }
   state_set_reg(out.set("last_io_count", total), "rax", total)
}

fn _state_writev_fd(dict st, int fd, int iov, int count) dict {
   mut out = st
   mut total = 0
   mut i = 0
   while i < count {
      def base = _mem_read_ptr(out, iov + i * 16, 0)
      def n = _mem_read_ptr(out, iov + i * 16 + 8, 0)
      if base <= 0 || n <= 0 { break }
      out = _state_send_fd(out, fd, base, n)
      total += int(out.get("last_io_count", 0))
      i += 1
   }
   state_set_reg(out.set("last_io_count", total), "rax", total)
}

fn _state_getenv(dict st, str name) dict {
   def env = state_env(st)
   if !env.contains(name) { return state_set_reg(st, "rax", 0).set("last_env_name", name) }
   def value = to_str(env.get(name, ""))
   def alloc = _heap_alloc(st, value.len + 1)
   state_set_reg(_write_cstr(alloc[0], alloc[1], value).set("last_env_name", name), "rax", alloc[1])
}

fn _aux_default(dict st, int typ) int {
   match typ {
      6 -> 4096
      11, 12 -> int(st.get("uid", 1000))
      13, 14 -> int(st.get("gid", 1000))
      17 -> 100
      23 -> 0
      25 -> int(st.get("aux_random", 0))
      31 -> int(st.get("aux_execfn", 0))
      _ -> 0
   }
}

fn _state_getauxval(dict st, int typ) dict {
   def aux = st.get("auxv", dict())
   def key = to_str(typ)
   def val = aux.contains(key) ? int(aux.get(key, 0)) : (aux.contains(typ) ? int(aux.get(typ, 0)) : _aux_default(st, typ))
   state_set_reg(st.set("last_auxv_type", typ), "rax", val)
}

fn _state_dlopen(dict st, str path, int flags) dict {
   def handle = max(1, int(st.get("next_dl_handle", 0x7f100000)))
   def rec = {"path": path, "flags": flags}
   state_set_reg(st.set("next_dl_handle", handle + 0x1000)
      .set("dl_handles", st.get("dl_handles", dict()).set(to_str(handle), rec))
      .set("last_dlopen_path", path), "rax", handle)
}

fn _state_dlsym(dict st, int handle, str name) dict {
   def syms = st.get("symbols", dict())
   mut addr = syms.contains(name) ? int(syms.get(name, 0)) : 0
   mut out = st
   if addr == 0 {
      addr = int(out.get("next_sym_addr", 0x7f200000))
      out = out.set("next_sym_addr", addr + 0x10)
   }
   state_set_reg(out.set("last_dlsym_handle", handle).set("last_dlsym_name", name), "rax", addr)
}

fn _state_fcntl(dict st, int fd, int cmd, int arg) dict {
   if cmd == 0 || cmd == 1030 { return _state_dup_fd(st, fd, max(arg, 3)).set("last_fcntl_cmd", cmd) }
   def rec = _fd_record(st, fd)
   if rec.len == 0 { return state_set_reg(st.set("last_fcntl_cmd", cmd), "rax", -9) }
   state_set_reg(st.set("last_fcntl_fd", fd).set("last_fcntl_cmd", cmd).set("last_fcntl_arg", arg), "rax", 0)
}

fn _state_ioctl(dict st, int fd, int req, int arg) dict {
   state_set_reg(st.set("last_ioctl_fd", fd).set("last_ioctl_req", req).set("last_ioctl_arg", arg), "rax", 0)
}

fn _state_time(dict st, int dst) dict {
   def sec = int(st.get("time", 1700000000))
   mut out = st
   if dst > 0 { out = _mem_write_i64(out, dst, sec) }
   state_set_reg(out, "rax", sec)
}

fn _state_gettimeofday(dict st, int tv, int tz) dict {
   mut out = st
   if tv > 0 {
      out = _mem_write_i64(out, tv, int(out.get("time", 1700000000)))
      out = _mem_write_i64(out, tv + 8, int(out.get("time_nsec", 0)) / 1000)
   }
   if tz > 0 {
      out = _mem_write_i32(out, tz, 0)
      out = _mem_write_i32(out, tz + 4, 0)
   }
   state_set_reg(out, "rax", 0)
}

fn _state_clock_gettime(dict st, int clk, int tp) dict {
   mut out = st
   if tp > 0 {
      out = _mem_write_i64(out, tp, int(out.get("time", 1700000000)))
      out = _mem_write_i64(out, tp + 8, int(out.get("time_nsec", 0)))
   }
   state_set_reg(out.set("last_clock_id", clk), "rax", 0)
}

fn _state_getrandom(dict st, int dst, int n, int flags) dict {
   if dst <= 0 || n <= 0 { return state_set_reg(st, "rax", 0) }
   def data = st.get("random", "")
   def total = _data_len(data)
   def pos = int(st.get("random_pos", 0))
   mut out = st
   mut i = 0
   while i < n {
      def b = total > 0 ? _item_at(data, (pos + i) % total) : 0
      out = mem_write_byte(out, dst + i, b)
      i += 1
   }
   state_set_reg(out.set("random_pos", pos + n).set("last_random_flags", flags).set("last_io_count", n), "rax", n)
}

fn _state_ptrace(dict st, int req, int pid, int addr, int data) dict {
   state_set_reg(st.set("last_ptrace_req", req).set("last_ptrace_pid", pid)
      .set("last_ptrace_addr", addr).set("last_ptrace_data", data), "rax", 0)
}

fn _state_prctl(dict st, int option, int arg2, int arg3, int arg4, int arg5) dict {
   state_set_reg(st.set("last_prctl_option", option).set("last_prctl_arg2", arg2)
      .set("last_prctl_arg3", arg3).set("last_prctl_arg4", arg4).set("last_prctl_arg5", arg5), "rax", 0)
}

fn _state_arch_prctl(dict st, int code, int addr) dict {
   mut out = st.set("last_arch_prctl_code", code).set("last_arch_prctl_addr", addr)
   if code == 0x1002 {
      out = out.set("tls_fs", addr)
   } elif code == 0x1003 {
      out = out.set("tls_gs", addr)
   } elif code == 0x1001 {
      if addr > 0 { out = _mem_write_i64(out, addr, int(out.get("tls_fs", 0))) }
   } elif code == 0x1004 {
      if addr > 0 { out = _mem_write_i64(out, addr, int(out.get("tls_gs", 0))) }
   }
   state_set_reg(out, "rax", 0)
}

fn _state_nanosleep(dict st, int req, int rem) dict {
   mut out = st.set("last_nanosleep_req", req).set("last_nanosleep_rem", rem)
   if rem > 0 {
      out = _mem_write_i64(out, rem, 0)
      out = _mem_write_i64(out, rem + 8, 0)
   }
   state_set_reg(out, "rax", 0)
}

fn _state_signal_result(dict st, str name, int a, int b=0, int c=0) dict {
   state_set_reg(st.set("last_signal_call", name).set("last_signal_a", a)
      .set("last_signal_b", b).set("last_signal_c", c), "rax", 0)
}

fn _state_fork_like(dict st, str name) dict {
   state_set_reg(st.set("last_process_call", name), "rax", int(st.get("fork_pid", 0)))
}

fn _state_execve(dict st, int path, int argv, int envp) dict {
   def p = path > 0 ? _read_cstring(st, path, 4096) : ""
   state_set_reg(st.set("last_execve_path", p).set("last_execve_argv", argv).set("last_execve_envp", envp), "rax", -2)
}

fn _state_wait(dict st, int pid, int status, int options, int rusage=0) dict {
   mut out = st.set("last_wait_pid", pid).set("last_wait_options", options).set("last_wait_rusage", rusage)
   if status > 0 { out = _mem_write_i32(out, status, 0) }
   state_set_reg(out, "rax", int(out.get("fork_pid", 0)))
}

fn _state_rand(dict st) dict {
   def seed = int(st.get("rand_seed", 1))
   def next = (seed * 1103515245 + 12345) & 0x7fffffff
   state_set_reg(st.set("rand_seed", next), "rax", (next >> 16) & 0x7fff)
}

fn _map_perms(int prot) str {
   mut p = ""
   if (prot & 1) != 0 { p = p + "r" }
   if (prot & 2) != 0 { p = p + "w" }
   if (prot & 4) != 0 { p = p + "x" }
   p.len == 0 ? "---" : p
}

fn _map_align(int n) int {
   ((max(1, n) + 4095) / 4096) * 4096
}

fn _state_map_record(dict st, dict rec) dict {
   st.set("maps", state_maps(st).append(rec))
}

fn _state_mmap(dict st, int addr, int length, int prot, int flags, int fd, int offset) dict {
   if length <= 0 { return state_set_reg(st, "rax", -22) }
   def size = _map_align(length)
   mut out = st
   mut base = addr
   if base == 0 {
      def alloc = _heap_alloc(out, size)
      out = alloc[0]
      base = int(alloc[1])
   }
   def rec = {"addr": base, "size": size, "length": length, "prot": prot,
      "perms": _map_perms(prot), "flags": flags, "fd": fd, "offset": offset,
      "active": true, "kind": fd >= 0 ? "file" : "anonymous"}
   if fd >= 0 {
      def frec = _fd_record(out, fd)
      if frec.len > 0 {
         def data = frec.get("data", "")
         def count = min(length, max(0, _data_len(data) - offset))
         mut i = 0
         while i < count {
            out = mem_write_byte(out, base + i, _item_at(data, offset + i))
            i += 1
         }
      }
   }
   state_set_reg(_state_map_record(out, rec).set("last_mmap", base), "rax", base)
}

fn _state_mprotect(dict st, int addr, int length, int prot) dict {
   def maps = state_maps(st)
   mut out_maps = []
   mut changed = false
   mut i = 0
   while i < maps.len {
      def m = maps[i]
      def ma = int(m.get("addr", 0))
      def ms = int(m.get("size", 0))
      if addr >= ma && addr < ma + ms {
         out_maps = out_maps.append(m.set("prot", prot).set("perms", _map_perms(prot)))
         changed = true
      } else {
         out_maps = out_maps.append(m)
      }
      i += 1
   }
   state_set_reg(st.set("maps", out_maps), "rax", changed ? 0 : -12)
}

fn _state_munmap(dict st, int addr, int length) dict {
   def maps = state_maps(st)
   mut out_maps = []
   mut changed = false
   mut i = 0
   while i < maps.len {
      def m = maps[i]
      if int(m.get("addr", 0)) == addr {
         out_maps = out_maps.append(m.set("active", false).set("unmapped_len", length))
         changed = true
      } else {
         out_maps = out_maps.append(m)
      }
      i += 1
   }
   state_set_reg(st.set("maps", out_maps), "rax", changed ? 0 : -22)
}

fn _state_brk(dict st, int requested) dict {
   def cur = int(st.get("brk", 0x91000000))
   if requested == 0 { return state_set_reg(st, "rax", cur) }
   state_set_reg(st.set("brk", requested), "rax", requested)
}

fn _state_read_line_stdin(dict st, int dst, int n, bool keep_newline=true) dict {
   def pos = int(st.get("stdin_pos", 0))
   def total = _state_input_len(st)
   def cap = max(0, n)
   mut count = 0
   mut out = st
   while count < cap && pos + count < total {
      def b = _state_input_at(out, pos + count)
      if is_int(b) && int(b) == 10 {
         if keep_newline {
            out = mem_write_byte(out, dst + count, b)
            count += 1
         }
         break
      }
      out = mem_write_byte(out, dst + count, b)
      count += 1
   }
   out.set("stdin_pos", pos + count).set("last_io_count", count).set("last_input_dst", dst).set("last_input_count", count)
}

fn _is_space_byte(any b) bool {
   if !is_int(b) { return false }
   def c = int(b) & 255
   c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32
}

fn _state_skip_stdin_space(dict st) dict {
   mut pos = int(st.get("stdin_pos", 0))
   def total = _state_input_len(st)
   while pos < total && _is_space_byte(_state_input_at(st, pos)) { pos += 1 }
   st.set("stdin_pos", pos)
}

fn _state_read_token_stdin(dict st, int dst, int cap) dict {
   mut out = _state_skip_stdin_space(st)
   def pos = int(out.get("stdin_pos", 0))
   def total = _state_input_len(out)
   mut count = 0
   while count < cap && pos + count < total {
      def b = _state_input_at(out, pos + count)
      if _is_space_byte(b) { break }
      out = mem_write_byte(out, dst + count, b)
      count += 1
   }
   out = mem_write_byte(out, dst + count, 0)
   out.set("stdin_pos", pos + count).set("last_io_count", count).set("last_input_dst", dst).set("last_input_count", count)
}

fn _scanf_specs(str fmt) list {
   mut out = []
   mut i = 0
   while i < fmt.len {
      if load8(fmt, i) != 37 { i += 1 continue }
      i += 1
      if i < fmt.len && load8(fmt, i) == 37 { i += 1 continue }
      mut suppress = false
      if i < fmt.len && load8(fmt, i) == 42 { suppress = true i += 1 }
      mut width = 0
      while i < fmt.len && str.ascii_is_digit(load8(fmt, i)) {
         width = width * 10 + (load8(fmt, i) - 48)
         i += 1
      }
      while i < fmt.len && contains("hljztL", str.chr(load8(fmt, i))) { i += 1 }
      if i >= fmt.len { break }
      def kind = str.chr(load8(fmt, i))
      i += 1
      if !suppress && contains("sdcxX", kind) {
         out = out.append({"kind": kind, "width": width})
      }
   }
   out
}

fn _state_scanf_stdin(dict st, str fmt, int first_arg) dict {
   def specs = _scanf_specs(fmt)
   mut out = st
   mut assigned = 0
   mut i = 0
   while i < specs.len {
      def spec = specs[i]
      def dst = int(_abi_arg(out, first_arg + i, 0))
      if dst == 0 { break }
      def kind = spec.get("kind", "")
      if kind == "s" {
         def width = int(spec.get("width", 0))
         out = _state_read_token_stdin(out, dst, width > 0 ? width : 1024)
         if int(out.get("last_io_count", 0)) <= 0 { break }
         assigned += 1
      } elif kind == "c" {
         def width = int(spec.get("width", 0))
         out = _state_read_stdin(out, dst, width > 0 ? width : 1)
         if int(out.get("last_io_count", 0)) <= 0 { break }
         assigned += 1
      } elif kind == "d" || kind == "x" || kind == "X" {
         out = _state_skip_stdin_space(out)
         def pos = int(out.get("stdin_pos", 0))
         def width = int(spec.get("width", 0))
         def base = kind == "d" ? 10 : 16
         def symbolic = _symbolic_parse_stdin_int_base(out, pos, width > 0 ? width : 10, base, 32)
         if symbolic.get("ok", false) {
            out = _state_assert_constraints(out, symbolic.get("constraints", []))
            out = _mem_write_i32(out, dst, symbolic.get("ast", 0))
            .set("stdin_pos", int(symbolic.get("pos", pos)) + int(symbolic.get("digits", 0)))
            assigned += 1
         } else {
            def input = state_stdin(out)
            def tail = pos < _data_len(input) ? slice(input, pos, _data_len(input), 1) : ""
            def parsed = _proc_parse_int(tail, base)
            if int(parsed.get("end", 0)) <= 0 { break }
            out = _mem_write_i32(out, dst, int(parsed.get("value", 0)))
            .set("stdin_pos", pos + int(parsed.get("end", 0)))
            assigned += 1
         }
      }
      i += 1
   }
   _abi_set_return(out, assigned)
}

fn _symbolic_decimal_limit(dict st, int addr, int max_digits) int {
   mut limit = max_digits <= 0 ? 10 : max_digits
   def rec = _symbolic_mem_record(st, addr)
   if rec.len > 0 {
      def off = addr - int(rec.get("addr", addr))
      def available = int(rec.get("count", limit)) - off
      if available > 0 { limit = min(limit, available) }
   }
   def last = int(st.get("last_input_dst", 0))
   def count = int(st.get("last_input_count", 0))
   if last != 0 && addr >= last && addr < last + count { limit = min(limit, last + count - addr) }
   max(0, limit)
}

fn _symbolic_parse_decimal(dict st, int addr, int max_digits=10) dict {
   _symbolic_parse_int_base(st, addr, max_digits, 10, 64)
}

fn _digit_value_for_char(int ch, int base) int {
   mut d = -1
   if ch >= 48 && ch <= 57 { d = ch - 48 }
   elif ch >= 65 && ch <= 90 { d = ch - 55 }
   elif ch >= 97 && ch <= 122 { d = ch - 87 }
   d >= 0 && d < base ? d : -1
}

fn _digit_value_ast(dict sol, any b, int base, int bits, str name) dict {
   def bbase = base <= 0 ? 10 : base
   def bbits = bits <= 0 ? 64 : bits
   if is_int(b) {
      def d = _digit_value_for_char(int(b) & 255, bbase)
      return d >= 0 ? {"ok": true, "ast": smt.bv_u64(sol.get("ctx", 0), d, bbits), "symbolic": false, "constraints": []} : {"ok": false}
   }
   if !_is_ast(b) || !sol.get("ok", false) { return {"ok": false} }
   def ctx = sol.get("ctx", 0)
   def dvar = bvs(sol, name, bbits)
   mut clauses = []
   mut v = 0
   while v < bbase && v < 10 {
      clauses = clauses.append(smt.mk_and(ctx, [
               smt.mk_eq(ctx, b, smt.bv_u8(ctx, 48 + v)),
               smt.mk_eq(ctx, dvar, smt.bv_u64(ctx, v, bbits)),
            ]))
      v += 1
   }
   v = 10
   while v < bbase && v < 16 {
      clauses = clauses.append(smt.mk_and(ctx, [
               smt.mk_eq(ctx, b, smt.bv_u8(ctx, 55 + v)),
               smt.mk_eq(ctx, dvar, smt.bv_u64(ctx, v, bbits)),
            ]))
      clauses = clauses.append(smt.mk_and(ctx, [
               smt.mk_eq(ctx, b, smt.bv_u8(ctx, 87 + v)),
               smt.mk_eq(ctx, dvar, smt.bv_u64(ctx, v, bbits)),
            ]))
      v += 1
   }
   clauses.len > 0 ? {"ok": true, "ast": dvar, "symbolic": true, "constraints": [smt.mk_or(ctx, clauses)]} : {"ok": false}
}

fn _symbolic_parse_int_base(dict st, int addr, int max_digits=10, int base=10, int bits=64) dict {
   def sol = state_solver(st)
   if !sol.get("ok", false) { return {"ok": false} }
   def ctx = sol.get("ctx", 0)
   mut p = addr
   mut sign = 1
   mut actual_base = base <= 0 ? 10 : base
   mut guard = 0
   while guard < 32 && _is_space_byte(_mem_read_byteish(st, p, -1)) { p += 1 guard += 1 }
   def first = _mem_read_byteish(st, p, -1)
   if is_int(first) && (int(first) == 43 || int(first) == 45) {
      sign = int(first) == 45 ? -1 : 1
      p += 1
   }
   if (base == 0 || base == 16) && _mem_read_byteish(st, p, -1) == 48 {
      def x = _mem_read_byteish(st, p + 1, -1)
      if is_int(x) && ((int(x) & 255) == 120 || (int(x) & 255) == 88) {
         actual_base = 16
         p += 2
      }
   }
   def limit = _symbolic_decimal_limit(st, p, max_digits)
   if limit <= 0 { return {"ok": false} }
   def bbits = bits <= 0 ? 64 : bits
   mut acc = smt.bv_u64(ctx, 0, bbits)
   mut cs = []
   mut digits = 0
   mut saw_symbolic = false
   mut i = 0
   while i < limit {
      def b = _mem_read_byteish(st, p + i, nil)
      def d = _digit_value_ast(sol, b, actual_base, bbits, "digit_" + to_str(addr) + "_" + to_str(i))
      if !d.get("ok", false) { break }
      if d.get("symbolic", false) { saw_symbolic = true }
      cs = _constraints_append_all(cs, d.get("constraints", []))
      acc = smt.bvadd(ctx, smt.bvmul(ctx, acc, smt.bv_u64(ctx, actual_base, bbits)), d.get("ast", 0))
      digits += 1
      i += 1
   }
   if digits == 0 || !saw_symbolic { return {"ok": false} }
   {"ok": true, "ast": sign < 0 ? smt.bvneg(ctx, acc) : acc, "constraints": cs, "digits": digits, "addr": p}
}

fn _symbolic_parse_stdin_decimal(dict st, int pos, int max_digits=10, int bits=32) dict {
   _symbolic_parse_stdin_int_base(st, pos, max_digits, 10, bits)
}

fn _symbolic_parse_stdin_int_base(dict st, int pos, int max_digits=10, int base=10, int bits=32) dict {
   def sol = state_solver(st)
   if !sol.get("ok", false) { return {"ok": false} }
   def ctx = sol.get("ctx", 0)
   def total = _state_input_len(st)
   mut p = pos
   mut sign = 1
   mut actual_base = base <= 0 ? 10 : base
   mut guard = 0
   while guard < 32 && p < total && _is_space_byte(_state_input_at(st, p)) { p += 1 guard += 1 }
   if p < total {
      def first = _state_input_at(st, p)
      if is_int(first) && (int(first) == 43 || int(first) == 45) {
         sign = int(first) == 45 ? -1 : 1
         p += 1
      }
   }
   if (base == 0 || base == 16) && p + 1 < total && _state_input_at(st, p) == 48 {
      def x = _state_input_at(st, p + 1)
      if is_int(x) && ((int(x) & 255) == 120 || (int(x) & 255) == 88) {
         actual_base = 16
         p += 2
      }
   }
   def bbits = bits <= 0 ? 32 : bits
   def limit = min(max_digits <= 0 ? 10 : max_digits, max(0, total - p))
   if limit <= 0 { return {"ok": false} }
   mut acc = smt.bv_u64(ctx, 0, bbits)
   mut cs = []
   mut digits = 0
   mut saw_symbolic = false
   mut i = 0
   while i < limit {
      def b = _state_input_at(st, p + i)
      def d = _digit_value_ast(sol, b, actual_base, bbits, "stdin_digit_" + to_str(pos) + "_" + to_str(i))
      if !d.get("ok", false) { break }
      if d.get("symbolic", false) { saw_symbolic = true }
      cs = _constraints_append_all(cs, d.get("constraints", []))
      acc = smt.bvadd(ctx, smt.bvmul(ctx, acc, smt.bv_u64(ctx, actual_base, bbits)), d.get("ast", 0))
      digits += 1
      i += 1
   }
   if digits == 0 || !saw_symbolic { return {"ok": false} }
   {"ok": true, "ast": sign < 0 ? smt.bvneg(ctx, acc) : acc, "constraints": cs, "digits": digits, "pos": p}
}

fn _proc_digit_value(int ch) int {
   if ch >= 48 && ch <= 57 { return ch - 48 }
   if ch >= 65 && ch <= 90 { return ch - 55 }
   if ch >= 97 && ch <= 122 { return ch - 87 }
   -1
}

fn _proc_parse_int(str s, int base=10) dict {
   mut i = 0
   while i < s.len {
      def c = load8(s, i) & 255
      if c != 32 && c != 9 && c != 10 && c != 13 { break }
      i += 1
   }
   mut sign = 1
   if i < s.len {
      def c = load8(s, i) & 255
      if c == 45 { sign = -1 i += 1 }
      elif c == 43 { i += 1 }
   }
   mut b = base
   if b == 0 { b = 10 }
   if (base == 0 || base == 16) && i + 1 < s.len && (load8(s, i) & 255) == 48 {
      def x = load8(s, i + 1) & 255
      if x == 120 || x == 88 { b = 16 i += 2 }
   }
   mut value = 0
   mut digits = 0
   while i < s.len {
      def d = _proc_digit_value(load8(s, i) & 255)
      if d < 0 || d >= b { break }
      value = value * b + d
      digits += 1
      i += 1
   }
   {"value": value * sign, "end": i, "digits": digits, "base": b}
}

fn _heap_base(dict st) int {
   int(st.get("heap_next", st.get("heap_base", 0x90000000)))
}

fn _heap_align(int n) int {
   ((max(1, n) + 15) / 16) * 16
}

fn _heap_alloc(dict st, int n) list {
   def size = _heap_align(n)
   def ptr = _heap_base(st)
   [st.set("heap_base", st.get("heap_base", 0x90000000)).set("heap_next", ptr + size), ptr, size]
}

fn _mem_snapshot(dict st, int addr, int n, any default=0) list {
   mut out = []
   mut i = 0
   while i < n {
      def b = _mem_byte_value(st, addr + i)
      out = out.append(b == nil ? default : b)
      i += 1
   }
   out
}

fn _mem_copy(dict st, int dst, int src, int n) dict {
   def bytes = _mem_snapshot(st, src, n, 0)
   mut out = st
   mut i = 0
   while i < bytes.len {
      out = mem_write_byte(out, dst + i, bytes[i])
      i += 1
   }
   out
}

fn _mem_copy_cstr(dict st, int dst, int src, int max_len=4096) dict {
   mut bytes = []
   mut scan = 0
   while scan < max_len {
      def b = _mem_byte_value(st, src + scan)
      bytes = bytes.append(b == nil ? 0 : b)
      if is_int(b) && int(b) == 0 { break }
      scan += 1
   }
   mut out = st
   mut i = 0
   while i < bytes.len {
      def b = bytes[i]
      out = mem_write_byte(out, dst + i, b)
      if is_int(b) && int(b) == 0 { return out }
      i += 1
   }
   out
}

fn _mem_set(dict st, int dst, int value, int n) dict {
   mut out = st
   mut i = 0
   while i < n {
      out = mem_write_byte(out, dst + i, value & 255)
      i += 1
   }
   out
}

fn _byte_xor(dict st, any byte, int key) any {
   if _is_ast(byte) {
      def ctx = state_solver(st).get("ctx", 0)
      return smt.bvxor(ctx, byte, smt.bv_u8(ctx, key & 255))
   }
   is_int(byte) ? ((int(byte) ^^ key) & 255) : byte
}

fn _mem_xor_in_place(dict st, int addr, int n, int key) dict {
   def bytes = _mem_snapshot(st, addr, n, 0)
   mut out = st
   mut i = 0
   while i < bytes.len {
      out = mem_write_byte(out, addr + i, _byte_xor(st, bytes[i], key))
      i += 1
   }
   out
}

fn _proc_uncompress(dict st, int dst, int dst_len_p, int src, int src_len) dict {
   if dst <= 0 || dst_len_p <= 0 || src <= 0 || src_len <= 0 { return _abi_set_return(st, -2) }
   def src_bytes = _concrete_mem_bytes(st, src, src_len)
   if !src_bytes.get("ok", false) { return _abi_set_return(st, -2) }
   def cap = max(0, _mem_read_ptr(st, dst_len_p, 0))
   if cap <= 0 { return _abi_set_return(st, -5) }
   def packed = mem_read_bytes(st, src, src_len)
   def plain = zlib.decompress_zlib_limit(packed, cap)
   if !plain || plain.len == 0 || zlib.error().len > 0 { return _abi_set_return(st, -3) }
   mut out = mem_write_bytes(st, dst, plain)
   out = _mem_write_ptr(out, dst_len_p, plain.len)
   _abi_set_return(out, 0)
}

fn _byte_eq_formula(dict st, any byte, int want) any {
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   if _is_ast(byte) { return smt.mk_eq(ctx, byte, smt.bv_u8(ctx, want & 255)) }
   if is_int(byte) {
      if (int(byte) & 255) == (want & 255) { return smt.mk_eq(ctx, smt.bv_u8(ctx, 0), smt.bv_u8(ctx, 0)) }
      return _false_ast(sol)
   }
   0
}

fn _symbolic_mem_search_return(dict st, str proc, int addr, int n, int needle) dict {
   if addr <= 0 || n <= 0 { return _abi_set_return(st, 0) }
   mut concrete_found = -1
   mut saw_symbolic = false
   mut eqs = []
   mut neqs = []
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   mut i = 0
   while i < n {
      def b = _mem_byte_value(st, addr + i)
      if is_int(b) && (int(b) & 255) == (needle & 255) {
         concrete_found = i
         break
      }
      def eq = _byte_eq_formula(st, b, needle)
      if _is_ast(eq) {
         if _is_ast(b) { saw_symbolic = true }
         eqs = eqs.append(eq)
         neqs = neqs.append(smt.mk_not(ctx, eq))
      }
      i += 1
   }
   if concrete_found >= 0 { return _abi_set_return(st, addr + concrete_found) }
   if saw_symbolic && eqs.len > 0 {
      return _proc_symbolic_return_nullness(st, proc,
         [smt.mk_and(ctx, neqs)],
         [smt.mk_or(ctx, eqs)])
   }
   _abi_set_return(st, 0)
}

fn _symbolic_substring_return(dict st, str proc, int hay_addr, int hay_n, list needle) dict {
   if hay_addr <= 0 { return _abi_set_return(st, 0) }
   if needle.len == 0 { return _abi_set_return(st, hay_addr) }
   if hay_n < needle.len { return _abi_set_return(st, 0) }
   def sol = state_solver(st)
   def ctx = sol.get("ctx", 0)
   mut matches = []
   mut saw_symbolic = false
   mut pos = 0
   while pos <= hay_n - needle.len {
      mut terms = []
      mut j = 0
      while j < needle.len {
         def b = _mem_byte_value(st, hay_addr + pos + j)
         if _is_ast(b) { saw_symbolic = true }
         def eq = _byte_eq_formula(st, b, int(needle[j]) & 255)
         if _is_ast(eq) { terms = terms.append(eq) }
         j += 1
      }
      if terms.len > 0 {
         matches = matches.append(terms.len == 1 ? terms[0] : smt.mk_and(ctx, terms))
      }
      pos += 1
   }
   if saw_symbolic && matches.len > 0 {
      mut zero_terms = []
      mut i = 0
      while i < matches.len {
         zero_terms = zero_terms.append(smt.mk_not(ctx, matches[i]))
         i += 1
      }
      return _proc_symbolic_return_nullness(st, proc,
         [zero_terms.len == 1 ? zero_terms[0] : smt.mk_and(ctx, zero_terms)],
         [matches.len == 1 ? matches[0] : smt.mk_or(ctx, matches)])
   }
   _abi_set_return(st, 0)
}

fn _cstring_search_limit(dict st, int addr, int max_len) int {
   if addr <= 0 || max_len <= 0 { return 0 }
   mut i = 0
   while i < max_len {
      def b = _mem_byte_value(st, addr + i)
      if is_int(b) && (int(b) & 255) == 0 { return i + 1 }
      if !_is_ast(b) && !is_int(b) { return i }
      i += 1
   }
   max_len
}

fn _apply_proc(dict st, dict hook) dict {
   def proc = _proc_name(hook.get("proc", ""))
   if proc.len == 0 { return st }
   mut out = st
   match proc {
      "puts" -> {
         def s = _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096)))
         out = state_append_stdout(out, s + "\n")
         out = _abi_set_return(out, s.len + 1)
      }
      "printf" -> {
         def s = _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096)))
         out = state_append_stdout(out, s)
         out = _abi_set_return(out, s.len)
      }
      "write" -> {
         def fd = int(_abi_arg(out, 0, -1))
         def ptr = int(_abi_arg(out, 1, 0))
         def n = int(_abi_arg(out, 2, 0))
         def s = mem_read_bytes(out, ptr, n)
         if fd == 1 { out = state_append_stdout(out, s) }
         if fd == 2 { out = state_append_stderr(out, s) }
         out = _abi_set_return(out, s.len)
      }
      "fwrite" -> {
         def ptr = int(_abi_arg(out, 0, 0))
         def size = int(_abi_arg(out, 1, 0))
         def nmemb = int(_abi_arg(out, 2, 0))
         def total = max(0, size * nmemb)
         def s = mem_read_bytes(out, ptr, total)
         out = _abi_set_return(state_append_stdout(out, s), nmemb)
      }
      "putchar" -> {
         def ch = int(_abi_arg(out, 0, 0)) & 255
         out = _abi_set_return(state_append_stdout(out, str.chr(ch)), ch)
      }
      "getchar" -> {
         def pos = int(out.get("stdin_pos", 0))
         if pos < _state_input_len(out) {
            def b = _state_input_at(out, pos)
            out = out.set("stdin_pos", pos + 1)
            if is_int(b) { out = _abi_set_return(out, int(b) & 255) }
            else { out = _abi_set_return(out, b) }
         } else {
            out = _abi_set_return(out, -1)
         }
      }
      "read" -> {
         def fd = int(_abi_arg(out, 0, -1))
         def dst = int(_abi_arg(out, 1, 0))
         def n = int(_abi_arg(out, 2, 0))
         if dst != 0 && n > 0 {
            out = _state_read_fd(out, fd, dst, n)
            out = _abi_set_return(out, int(out.get("last_io_count", 0)))
         } else {
            out = _abi_set_return(out, 0)
         }
      }
      "open" -> {
         def opened = _state_open_file(out, _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096))))
         out = _abi_set_return(opened, int(opened.get("last_fd", -2)))
      }
      "openat" -> {
         def opened = _state_open_file(out, _read_cstring(out, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096))))
         out = _abi_set_return(opened, int(opened.get("last_fd", -2)))
      }
      "close" -> {
         out = _abi_set_return(_fd_drop(out, int(_abi_arg(out, 0, -1))), 0)
      }
      "dup" -> {
         def du = _state_dup_fd(out, int(_abi_arg(out, 0, -1)))
         out = _abi_set_return(du, int(state_get_reg(du, "rax", -9)))
      }
      "dup2", "dup3" -> {
         def du = _state_dup_fd(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, -1)))
         out = _abi_set_return(du, int(state_get_reg(du, "rax", -9)))
      }
      "pipe", "pipe2" -> {
         def pp = _state_pipe(out, int(_abi_arg(out, 0, 0)))
         out = _abi_set_return(pp, int(state_get_reg(pp, "rax", -14)))
      }
      "fcntl" -> {
         def fc = _state_fcntl(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(fc, int(state_get_reg(fc, "rax", -9)))
      }
      "ioctl" -> {
         def io = _state_ioctl(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(io, int(state_get_reg(io, "rax", 0)))
      }
      "readv" -> {
         def rv = _state_readv_fd(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(rv, int(state_get_reg(rv, "rax", 0)))
      }
      "writev" -> {
         def wv = _state_writev_fd(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(wv, int(state_get_reg(wv, "rax", 0)))
      }
      "lseek" -> {
         out = _state_seek_fd(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
      }
      "access" -> {
         def acc = _state_access_path(out, _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096))))
         out = _abi_set_return(acc, int(state_get_reg(acc, "rax", -2)))
      }
      "faccessat" -> {
         def acc = _state_access_path(out, _read_cstring(out, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096))))
         out = _abi_set_return(acc, int(state_get_reg(acc, "rax", -2)))
      }
      "stat", "lstat" -> {
         def stp = _state_stat_path(out, _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096))), int(_abi_arg(out, 1, 0)))
         out = _abi_set_return(stp, int(state_get_reg(stp, "rax", -2)))
      }
      "fstat" -> {
         def fst = _state_fstat_fd(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)))
         out = _abi_set_return(fst, int(state_get_reg(fst, "rax", -9)))
      }
      "newfstatat" -> {
         def stp = _state_stat_path(out, _read_cstring(out, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096))), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(stp, int(state_get_reg(stp, "rax", -2)))
      }
      "readlink" -> {
         def rlk = _state_readlink_path(out, _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096))), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(rlk, int(state_get_reg(rlk, "rax", -22)))
      }
      "readlinkat" -> {
         def rlk = _state_readlink_path(out, _read_cstring(out, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096))), int(_abi_arg(out, 2, 0)), int(_abi_arg(out, 3, 0)))
         out = _abi_set_return(rlk, int(state_get_reg(rlk, "rax", -22)))
      }
      "uname" -> {
         def un = _state_uname(out, int(_abi_arg(out, 0, 0)))
         out = _abi_set_return(un, int(state_get_reg(un, "rax", -14)))
      }
      "mmap" -> {
         out = _state_mmap(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)),
            int(_abi_arg(out, 2, 0)), int(_abi_arg(out, 3, 0)),
            int(_abi_arg(out, 4, -1)), int(_abi_arg(out, 5, 0)))
      }
      "mprotect" -> {
         out = _state_mprotect(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
      }
      "munmap" -> {
         out = _state_munmap(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)))
      }
      "brk" -> {
         out = _state_brk(out, int(_abi_arg(out, 0, 0)))
      }
      "socket" -> {
         def sock = _state_socket(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(sock, int(sock.get("last_fd", -1)))
      }
      "connect" -> {
         def connected = _state_connect_fd(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(connected, int(state_get_reg(connected, "rax", 0)))
      }
      "recv", "recvfrom" -> {
         def fd = int(_abi_arg(out, 0, -1))
         def dst = int(_abi_arg(out, 1, 0))
         def n = int(_abi_arg(out, 2, 0))
         out = _state_recv_fd(out, fd, dst, n)
         out = _abi_set_return(out, int(out.get("last_io_count", 0)))
      }
      "send", "sendto" -> {
         def fd = int(_abi_arg(out, 0, -1))
         def src = int(_abi_arg(out, 1, 0))
         def n = int(_abi_arg(out, 2, 0))
         out = _state_send_fd(out, fd, src, n)
         out = _abi_set_return(out, int(out.get("last_io_count", 0)))
      }
      "getenv" -> {
         def envd = _state_getenv(out, _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096))))
         out = _abi_set_return(envd, int(state_get_reg(envd, "rax", 0)))
      }
      "getauxval" -> {
         def ax = _state_getauxval(out, int(_abi_arg(out, 0, 0)))
         out = _abi_set_return(ax, int(state_get_reg(ax, "rax", 0)))
      }
      "dlopen" -> {
         def dl = _state_dlopen(out, _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096))), int(_abi_arg(out, 1, 0)))
         out = _abi_set_return(dl, int(state_get_reg(dl, "rax", 0)))
      }
      "dlsym" -> {
         def ds = _state_dlsym(out, int(_abi_arg(out, 0, 0)), _read_cstring(out, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096))))
         out = _abi_set_return(ds, int(state_get_reg(ds, "rax", 0)))
      }
      "dlclose" -> {
         out = _abi_set_return(out.set("last_dlclose_handle", int(_abi_arg(out, 0, 0))), 0)
      }
      "dlerror" -> {
         out = _abi_set_return(out, 0)
      }
      "time" -> {
         def timed = _state_time(out, int(_abi_arg(out, 0, 0)))
         out = _abi_set_return(timed, int(state_get_reg(timed, "rax", 0)))
      }
      "gettimeofday" -> {
         def timed = _state_gettimeofday(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)))
         out = _abi_set_return(timed, int(state_get_reg(timed, "rax", 0)))
      }
      "clock_gettime" -> {
         def timed = _state_clock_gettime(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)))
         out = _abi_set_return(timed, int(state_get_reg(timed, "rax", 0)))
      }
      "getpid" -> {
         out = _abi_set_return(out, int(out.get("pid", 1337)))
      }
      "gettid" -> {
         out = _abi_set_return(out, int(out.get("tid", out.get("pid", 1337))))
      }
      "getppid" -> {
         out = _abi_set_return(out, int(out.get("ppid", 1)))
      }
      "getuid", "geteuid" -> {
         out = _abi_set_return(out, int(out.get("uid", 1000)))
      }
      "getgid", "getegid" -> {
         out = _abi_set_return(out, int(out.get("gid", 1000)))
      }
      "getrandom" -> {
         def rnd = _state_getrandom(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(rnd, int(rnd.get("last_io_count", 0)))
      }
      "ptrace" -> {
         def pt = _state_ptrace(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)),
            int(_abi_arg(out, 2, 0)), int(_abi_arg(out, 3, 0)))
         out = _abi_set_return(pt, int(state_get_reg(pt, "rax", 0)))
      }
      "prctl" -> {
         def pc = _state_prctl(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)),
            int(_abi_arg(out, 2, 0)), int(_abi_arg(out, 3, 0)), int(_abi_arg(out, 4, 0)))
         out = _abi_set_return(pc, int(state_get_reg(pc, "rax", 0)))
      }
      "arch_prctl" -> {
         def ap = _state_arch_prctl(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)))
         out = _abi_set_return(ap, int(state_get_reg(ap, "rax", 0)))
      }
      "sleep", "usleep" -> {
         out = _abi_set_return(out.set("last_sleep_arg", int(_abi_arg(out, 0, 0))), 0)
      }
      "nanosleep" -> {
         def ns = _state_nanosleep(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)))
         out = _abi_set_return(ns, int(state_get_reg(ns, "rax", 0)))
      }
      "kill", "raise" -> {
         def sig = proc == "raise" ? int(_abi_arg(out, 0, 0)) : int(_abi_arg(out, 1, 0))
         def pid = proc == "raise" ? int(out.get("pid", 1337)) : int(_abi_arg(out, 0, 0))
         def sg = _state_signal_result(out, proc, pid, sig)
         out = _abi_set_return(sg, int(state_get_reg(sg, "rax", 0)))
      }
      "tgkill" -> {
         def sg = _state_signal_result(out, "tgkill", int(_abi_arg(out, 0, 0)),
            int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(sg, int(state_get_reg(sg, "rax", 0)))
      }
      "fork", "vfork" -> {
         def fk = _state_fork_like(out, proc)
         out = _abi_set_return(fk, int(state_get_reg(fk, "rax", 0)))
      }
      "clone" -> {
         def cl = _state_fork_like(out.set("last_clone_flags", int(_abi_arg(out, 0, 0)))
            .set("last_clone_stack", int(_abi_arg(out, 1, 0))), "clone")
         out = _abi_set_return(cl, int(state_get_reg(cl, "rax", 0)))
      }
      "execve" -> {
         def ex = _state_execve(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(ex, int(state_get_reg(ex, "rax", -2)))
      }
      "wait", "waitpid" -> {
         def wt = _state_wait(out, proc == "wait" ? -1 : int(_abi_arg(out, 0, -1)),
            proc == "wait" ? int(_abi_arg(out, 0, 0)) : int(_abi_arg(out, 1, 0)),
            proc == "wait" ? 0 : int(_abi_arg(out, 2, 0)), 0)
         out = _abi_set_return(wt, int(state_get_reg(wt, "rax", 0)))
      }
      "wait4" -> {
         def wt = _state_wait(out, int(_abi_arg(out, 0, -1)), int(_abi_arg(out, 1, 0)),
            int(_abi_arg(out, 2, 0)), int(_abi_arg(out, 3, 0)))
         out = _abi_set_return(wt, int(state_get_reg(wt, "rax", 0)))
      }
      "system" -> {
         out = _abi_set_return(out.set("last_system_cmd", _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096)))), 0)
      }
      "srand" -> {
         out = out.set("rand_seed", int(_abi_arg(out, 0, 1)))
      }
      "rand" -> {
         def rnd = _state_rand(out)
         out = _abi_set_return(rnd, int(state_get_reg(rnd, "rax", 0)))
      }
      "fgets" -> {
         def dst = int(_abi_arg(out, 0, 0))
         def n = int(_abi_arg(out, 1, 0))
         if dst != 0 && n > 1 {
            out = _state_read_line_stdin(out, dst, n - 1, true)
            def got = int(out.get("last_io_count", 0))
            if got > 0 {
               out = mem_write_byte(out, dst + got, 0)
               out = _abi_set_return(out, dst)
            } else {
               out = _abi_set_return(out, 0)
            }
         } else {
            out = _abi_set_return(out, 0)
         }
      }
      "gets" -> {
         def dst = int(_abi_arg(out, 0, 0))
         if dst != 0 {
            out = _state_read_line_stdin(out, dst, max(0, _state_input_len(out) - int(out.get("stdin_pos", 0))), false)
            def got = int(out.get("last_io_count", 0))
            out = mem_write_byte(out, dst + got, 0)
            out = _abi_set_return(out, dst)
         } else {
            out = _abi_set_return(out, 0)
         }
      }
      "scanf" -> {
         def fmt = _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096)))
         out = _state_scanf_stdin(out, fmt, 1)
      }
      "fscanf" -> {
         def fmt = _read_cstring(out, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096)))
         out = _state_scanf_stdin(out, fmt, 2)
      }
      "sscanf" -> {
         def src = int(_abi_arg(out, 0, 0))
         def fmt = _read_cstring(out, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096)))
         out = _state_scanf_stdin(out.set("stdin", _read_cstring(out, src, int(hook.get("max_string", 4096)))).set("stdin_pos", 0), fmt, 2)
      }
      "strlen" -> {
         def s = _read_cstring(out, int(_abi_arg(out, 0, 0)), int(hook.get("max_string", 4096)))
         out = _abi_set_return(out, s.len)
      }
      "atoi", "atol", "atoll" -> {
         def src = int(_abi_arg(out, 0, 0))
         def symbolic = _symbolic_parse_decimal(out, src, int(hook.get("max_digits", 10)))
         if symbolic.get("ok", false) {
            out = _state_assert_constraints(out, symbolic.get("constraints", []))
            out = _abi_set_return(out, symbolic.get("ast", 0))
         } else {
            def s = _read_cstring(out, src, int(hook.get("max_string", 4096)))
            out = _abi_set_return(out, _proc_parse_int(s, 10).get("value", 0))
         }
      }
      "strtol", "strtoll" -> {
         def src = int(_abi_arg(out, 0, 0))
         def endp = int(_abi_arg(out, 1, 0))
         def base = int(_abi_arg(out, 2, 10))
         def symbolic = base == 0 || base == 10 || base == 16 ? _symbolic_parse_int_base(out, src, int(hook.get("max_digits", 10)), base, 64) : {"ok": false}
         if symbolic.get("ok", false) {
            out = _state_assert_constraints(out, symbolic.get("constraints", []))
            if endp > 0 { out = _mem_write_ptr(out, endp, int(symbolic.get("addr", src)) + int(symbolic.get("digits", 0))) }
            out = _abi_set_return(out, symbolic.get("ast", 0))
         } else {
            def parsed = _proc_parse_int(_read_cstring(out, src, int(hook.get("max_string", 4096))), base)
            if endp > 0 { out = _mem_write_ptr(out, endp, src + int(parsed.get("end", 0))) }
            out = _abi_set_return(out, parsed.get("value", 0))
         }
      }
      "strcmp" -> {
         def ap = int(_abi_arg(out, 0, 0))
         def bp = int(_abi_arg(out, 1, 0))
         def maxs = int(hook.get("max_string", 4096))
         def ab = _concrete_cstring_bytes(out, ap, maxs)
         def bb = _concrete_cstring_bytes(out, bp, maxs)
         if ab.get("ok", false) && bb.get("ok", false) {
            out = _abi_set_return(out, _proc_cmp(_read_cstring(out, ap, maxs), _read_cstring(out, bp, maxs)))
         } elif bb.get("ok", false) {
            out = _proc_symbolic_return_eqness(out, "strcmp", _mem_eq_constraints(out, _symbolic_compare_addr(out, ap), bb.get("bytes", [])))
         } elif ab.get("ok", false) {
            out = _proc_symbolic_return_eqness(out, "strcmp", _mem_eq_constraints(out, _symbolic_compare_addr(out, bp), ab.get("bytes", [])))
         } else {
            out = _proc_symbolic_return(out, "strcmp")
         }
      }
      "strcasecmp" -> {
         def ap = int(_abi_arg(out, 0, 0))
         def bp = int(_abi_arg(out, 1, 0))
         def maxs = int(hook.get("max_string", 4096))
         out = _abi_set_return(out, _proc_cmp(_ascii_lower_str(_read_cstring(out, ap, maxs)),
               _ascii_lower_str(_read_cstring(out, bp, maxs))))
      }
      "strncmp" -> {
         def ap = int(_abi_arg(out, 0, 0))
         def bp = int(_abi_arg(out, 1, 0))
         def n = max(0, int(_abi_arg(out, 2, 0)))
         def ab = _concrete_mem_bytes(out, ap, n)
         def bb = _concrete_mem_bytes(out, bp, n)
         if ab.get("ok", false) && bb.get("ok", false) {
            out = _abi_set_return(out, _proc_cmp(mem_read_bytes(out, ap, n), mem_read_bytes(out, bp, n), n))
         } elif bb.get("ok", false) {
            out = _proc_symbolic_return_eqness(out, "strncmp", _mem_eq_constraints(out, _symbolic_compare_addr(out, ap), bb.get("bytes", [])))
         } elif ab.get("ok", false) {
            out = _proc_symbolic_return_eqness(out, "strncmp", _mem_eq_constraints(out, _symbolic_compare_addr(out, bp), ab.get("bytes", [])))
         } else {
            out = _proc_symbolic_return_eqness(out, "strncmp", _mem_pair_eq_constraints(out, ap, bp, n))
         }
      }
      "strncasecmp" -> {
         def ap = int(_abi_arg(out, 0, 0))
         def bp = int(_abi_arg(out, 1, 0))
         def n = max(0, int(_abi_arg(out, 2, 0)))
         out = _abi_set_return(out, _proc_cmp(_ascii_lower_str(mem_read_bytes(out, ap, n)),
               _ascii_lower_str(mem_read_bytes(out, bp, n)), n))
      }
      "memcmp", "bcmp" -> {
         def ap = int(_abi_arg(out, 0, 0))
         def bp = int(_abi_arg(out, 1, 0))
         def n = max(0, int(_abi_arg(out, 2, 0)))
         def ab = _concrete_mem_bytes(out, ap, n)
         def bb = _concrete_mem_bytes(out, bp, n)
         if ab.get("ok", false) && bb.get("ok", false) {
            out = _abi_set_return(out, _proc_memcmp(out, ap, bp, n))
         } elif bb.get("ok", false) {
            out = _proc_symbolic_return_eqness(out, "memcmp", _mem_eq_constraints(out, _symbolic_compare_addr(out, ap), bb.get("bytes", [])))
         } elif ab.get("ok", false) {
            out = _proc_symbolic_return_eqness(out, "memcmp", _mem_eq_constraints(out, _symbolic_compare_addr(out, bp), ab.get("bytes", [])))
         } else {
            out = _proc_symbolic_return_eqness(out, "memcmp", _mem_pair_eq_constraints(out, ap, bp, n))
         }
      }
      "memchr" -> {
         out = _symbolic_mem_search_return(out, "memchr", int(_abi_arg(out, 0, 0)),
            max(0, int(_abi_arg(out, 2, 0))), int(_abi_arg(out, 1, 0)))
      }
      "strchr" -> {
         def src = int(_abi_arg(out, 0, 0))
         def limit = _cstring_search_limit(out, src, int(hook.get("max_string", 4096)))
         out = _symbolic_mem_search_return(out, "strchr", src, limit, int(_abi_arg(out, 1, 0)))
      }
      "strrchr" -> {
         def src = int(_abi_arg(out, 0, 0))
         def needle = int(_abi_arg(out, 1, 0))
         def limit = _cstring_search_limit(out, src, int(hook.get("max_string", 4096)))
         mut found = -1
         mut i = 0
         while i < limit {
            def b = _mem_byte_value(out, src + i)
            if is_int(b) && (int(b) & 255) == (needle & 255) { found = i }
            i += 1
         }
         out = found >= 0 ? _abi_set_return(out, src + found) : _symbolic_mem_search_return(out, "strrchr", src, limit, needle)
      }
      "strstr" -> {
         def hay_addr = int(_abi_arg(out, 0, 0))
         def needle_addr = int(_abi_arg(out, 1, 0))
         def maxs = int(hook.get("max_string", 4096))
         def hay_bytes = _concrete_cstring_bytes(out, hay_addr, maxs)
         def needle_bytes = _concrete_cstring_bytes(out, needle_addr, maxs)
         if hay_bytes.get("ok", false) && needle_bytes.get("ok", false) {
            def hay = _read_cstring(out, hay_addr, maxs)
            def needle = _read_cstring(out, needle_addr, maxs)
            def pos = str.find(hay, needle)
            out = _abi_set_return(out, pos >= 0 ? hay_addr + pos : 0)
         } elif needle_bytes.get("ok", false) {
            def nb = needle_bytes.get("bytes", [])
            def needle_len = nb.len > 0 && int(nb[nb.len - 1]) == 0 ? nb.len - 1 : nb.len
            out = _symbolic_substring_return(out, "strstr", _symbolic_compare_addr(out, hay_addr),
               _cstring_search_limit(out, hay_addr, maxs), slice(nb, 0, needle_len, 1))
         } else {
            out = _proc_symbolic_return(out, "strstr")
         }
      }
      "memcpy" -> {
         def dst = int(_abi_arg(out, 0, 0))
         out = _abi_set_return(_mem_copy(out, dst, int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0))), dst)
      }
      "memmove" -> {
         def dst = int(_abi_arg(out, 0, 0))
         out = _abi_set_return(_mem_copy(out, dst, int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0))), dst)
      }
      "memset" -> {
         def dst = int(_abi_arg(out, 0, 0))
         out = _abi_set_return(_mem_set(out, dst, int(_abi_arg(out, 1, 0)), int(_abi_arg(out, 2, 0))), dst)
      }
      "memfrob" -> {
         def ptr = int(_abi_arg(out, 0, 0))
         out = _abi_set_return(_mem_xor_in_place(out, ptr, max(0, int(_abi_arg(out, 1, 0))), 42), ptr)
      }
      "strcpy" -> {
         def dst = int(_abi_arg(out, 0, 0))
         out = _abi_set_return(_mem_copy_cstr(out, dst, int(_abi_arg(out, 1, 0)), int(hook.get("max_string", 4096))), dst)
      }
      "strncpy" -> {
         def dst = int(_abi_arg(out, 0, 0))
         def src = int(_abi_arg(out, 1, 0))
         def n = max(0, int(_abi_arg(out, 2, 0)))
         mut copied = 0
         while copied < n {
            def b = mem_read(out, src + copied, 0)
            out = mem_write_byte(out, dst + copied, b)
            copied += 1
            if is_int(b) && int(b) == 0 { break }
         }
         while copied < n {
            out = mem_write_byte(out, dst + copied, 0)
            copied += 1
         }
         out = _abi_set_return(out, dst)
      }
      "strdup" -> {
         def src = int(_abi_arg(out, 0, 0))
         def bytes = _concrete_cstring_bytes(out, src, int(hook.get("max_string", 4096)))
         def need = bytes.get("ok", false) ? bytes.get("bytes", []).len : (_read_cstring(out, src, int(hook.get("max_string", 4096))).len + 1)
         def alloc = _heap_alloc(out, need)
         out = _mem_copy_cstr(alloc[0], alloc[1], src, int(hook.get("max_string", 4096)))
         out = _abi_set_return(out, alloc[1])
      }
      "uncompress" -> {
         out = _proc_uncompress(out, int(_abi_arg(out, 0, 0)), int(_abi_arg(out, 1, 0)),
            int(_abi_arg(out, 2, 0)), int(_abi_arg(out, 3, 0)))
      }
      "malloc" -> {
         def alloc = _heap_alloc(out, int(_abi_arg(out, 0, 0)))
         out = _abi_set_return(alloc[0], alloc[1])
      }
      "calloc" -> {
         def total = max(0, int(_abi_arg(out, 0, 0)) * int(_abi_arg(out, 1, 0)))
         def alloc = _heap_alloc(out, total)
         out = _abi_set_return(_mem_set(alloc[0], alloc[1], 0, total), alloc[1])
      }
      "realloc" -> {
         def oldp = int(_abi_arg(out, 0, 0))
         def n = int(_abi_arg(out, 1, 0))
         def alloc = _heap_alloc(out, n)
         out = _abi_set_return(_mem_copy(alloc[0], alloc[1], oldp, n), alloc[1])
      }
      "free" -> {
         out = _abi_set_return(out, 0)
      }
      "exit", "abort" -> {
         out = out.set("deadended", true)
      }
      _ -> { }
   }
   out
}

fn _apply_hook(dict st, dict hook) dict {
   def pc = state_addr(st)
   mut out = _apply_proc(state_clone(st), hook)
   def regs = hook.get("regs", dict())
   def rks = regs.keys()
   mut i = 0
   while i < rks.len {
      def k = rks[i]
      out = state_set_reg(out, k, regs.get(k, 0))
      i += 1
   }
   def mem = hook.get("mem", dict())
   def mks = mem.keys()
   i = 0
   while i < mks.len {
      def k = mks[i]
      out = mem_write(out, int(k), mem.get(k, ""))
      i += 1
   }
   if hook.contains("stdout") { out = state_append_stdout(out, hook.get("stdout", "")) }
   if hook.contains("stderr") { out = state_append_stderr(out, hook.get("stderr", "")) }
   def next = int(hook.get("ret", pc + int(hook.get("size", 1))))
   out = _state_set_pc(out, next)
   if hook.get("deadend", false) { out = out.set("deadended", true) }
   _history_append(out.set("step_ok", true).set("step_reason", "ok"),
      {"addr": pc, "next": next, "backend": "hook", "hook": hook.get("name", "hook")})
}

fn _apply_linux_x86_64_syscall(dict st) dict {
   def nr = int(state_get_reg(st, "rax", 0))
   mut out = st
   match nr {
      0 -> {
         def fd = int(state_get_reg(out, "rdi", -1))
         def dst = int(state_get_reg(out, "rsi", 0))
         def n = int(state_get_reg(out, "rdx", 0))
         if dst > 0 && n > 0 {
            out = _state_read_fd(out, fd, dst, n)
            out = state_set_reg(out, "rax", int(out.get("last_io_count", 0)))
         } else {
            out = state_set_reg(out, "rax", 0)
         }
      }
      2 -> {
         def path = _read_cstring(out, int(state_get_reg(out, "rdi", 0)), 4096)
         def opened = _state_open_file(out, path)
         out = state_set_reg(opened, "rax", int(opened.get("last_fd", -2)))
      }
      3 -> {
         out = state_set_reg(_fd_drop(out, int(state_get_reg(out, "rdi", -1))), "rax", 0)
      }
      8 -> {
         out = _state_seek_fd(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      9 -> {
         out = _state_mmap(out, int(state_get_reg(out, "rdi", 0)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)),
            int(state_get_reg(out, "r10", 0)), int(state_get_reg(out, "r8", -1)),
            int(state_get_reg(out, "r9", 0)))
      }
      10 -> {
         out = _state_mprotect(out, int(state_get_reg(out, "rdi", 0)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      11 -> {
         out = _state_munmap(out, int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)))
      }
      12 -> {
         out = _state_brk(out, int(state_get_reg(out, "rdi", 0)))
      }
      16 -> {
         out = _state_ioctl(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      19 -> {
         out = _state_readv_fd(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      20 -> {
         out = _state_writev_fd(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      21 -> {
         out = _state_access_path(out, _read_cstring(out, int(state_get_reg(out, "rdi", 0)), 4096))
      }
      22 -> {
         out = _state_pipe(out, int(state_get_reg(out, "rdi", 0)))
      }
      24, 28 -> {
         out = state_set_reg(out, "rax", 0)
      }
      32 -> {
         out = _state_dup_fd(out, int(state_get_reg(out, "rdi", -1)))
      }
      33 -> {
         out = _state_dup_fd(out, int(state_get_reg(out, "rdi", -1)), int(state_get_reg(out, "rsi", -1)))
      }
      35 -> {
         out = _state_nanosleep(out, int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)))
      }
      39 -> {
         out = state_set_reg(out, "rax", int(out.get("pid", 1337)))
      }
      41 -> {
         def sock = _state_socket(out, int(state_get_reg(out, "rdi", 0)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
         out = state_set_reg(sock, "rax", int(sock.get("last_fd", -1)))
      }
      42 -> {
         def connected = _state_connect_fd(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
         out = state_set_reg(connected, "rax", int(state_get_reg(connected, "rax", 0)))
      }
      44 -> {
         out = _state_send_fd(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
         out = state_set_reg(out, "rax", int(out.get("last_io_count", 0)))
      }
      45 -> {
         out = _state_recv_fd(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
         out = state_set_reg(out, "rax", int(out.get("last_io_count", 0)))
      }
      96 -> {
         out = _state_gettimeofday(out, int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)))
      }
      4 -> {
         out = _state_stat_path(out, _read_cstring(out, int(state_get_reg(out, "rdi", 0)), 4096), int(state_get_reg(out, "rsi", 0)))
      }
      5 -> {
         out = _state_fstat_fd(out, int(state_get_reg(out, "rdi", -1)), int(state_get_reg(out, "rsi", 0)))
      }
      6 -> {
         out = _state_stat_path(out, _read_cstring(out, int(state_get_reg(out, "rdi", 0)), 4096), int(state_get_reg(out, "rsi", 0)))
      }
      63 -> {
         out = _state_uname(out, int(state_get_reg(out, "rdi", 0)))
      }
      89 -> {
         out = _state_readlink_path(out, _read_cstring(out, int(state_get_reg(out, "rdi", 0)), 4096),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      56 -> {
         out = _state_fork_like(out.set("last_clone_flags", int(state_get_reg(out, "rdi", 0)))
            .set("last_clone_stack", int(state_get_reg(out, "rsi", 0))), "clone")
      }
      57 -> {
         out = _state_fork_like(out, "fork")
      }
      58 -> {
         out = _state_fork_like(out, "vfork")
      }
      59 -> {
         out = _state_execve(out, int(state_get_reg(out, "rdi", 0)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      61 -> {
         out = _state_wait(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)),
            int(state_get_reg(out, "r10", 0)))
      }
      62 -> {
         out = _state_signal_result(out, "kill", int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)))
      }
      101 -> {
         out = _state_ptrace(out, int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)),
            int(state_get_reg(out, "rdx", 0)), int(state_get_reg(out, "r10", 0)))
      }
      102, 107 -> {
         out = state_set_reg(out, "rax", int(out.get("uid", 1000)))
      }
      104, 108 -> {
         out = state_set_reg(out, "rax", int(out.get("gid", 1000)))
      }
      110 -> {
         out = state_set_reg(out, "rax", int(out.get("ppid", 1)))
      }
      72 -> {
         out = _state_fcntl(out, int(state_get_reg(out, "rdi", -1)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      157 -> {
         out = _state_prctl(out, int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)),
            int(state_get_reg(out, "rdx", 0)), int(state_get_reg(out, "r10", 0)), int(state_get_reg(out, "r8", 0)))
      }
      158 -> {
         out = _state_arch_prctl(out, int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)))
      }
      186 -> {
         out = state_set_reg(out, "rax", int(out.get("tid", out.get("pid", 1337))))
      }
      201 -> {
         out = _state_time(out, int(state_get_reg(out, "rdi", 0)))
      }
      228 -> {
         out = _state_clock_gettime(out, int(state_get_reg(out, "rdi", 0)), int(state_get_reg(out, "rsi", 0)))
      }
      234 -> {
         out = _state_signal_result(out, "tgkill", int(state_get_reg(out, "rdi", 0)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      318 -> {
         out = _state_getrandom(out, int(state_get_reg(out, "rdi", 0)),
            int(state_get_reg(out, "rsi", 0)), int(state_get_reg(out, "rdx", 0)))
      }
      1 -> {
         def fd = int(state_get_reg(out, "rdi", -1))
         def ptr = int(state_get_reg(out, "rsi", 0))
         def n = int(state_get_reg(out, "rdx", 0))
         def s = mem_read_bytes(out, ptr, n)
         if fd == 1 { out = state_append_stdout(out, s) }
         if fd == 2 { out = state_append_stderr(out, s) }
         out = state_set_reg(out, "rax", s.len)
      }
      257 -> {
         def path = _read_cstring(out, int(state_get_reg(out, "rsi", 0)), 4096)
         def opened = _state_open_file(out, path)
         out = state_set_reg(opened, "rax", int(opened.get("last_fd", -2)))
      }
      262 -> {
         out = _state_stat_path(out, _read_cstring(out, int(state_get_reg(out, "rsi", 0)), 4096), int(state_get_reg(out, "rdx", 0)))
      }
      267 -> {
         out = _state_readlink_path(out, _read_cstring(out, int(state_get_reg(out, "rsi", 0)), 4096),
            int(state_get_reg(out, "rdx", 0)), int(state_get_reg(out, "r10", 0)))
      }
      292 -> {
         out = _state_dup_fd(out, int(state_get_reg(out, "rdi", -1)), int(state_get_reg(out, "rsi", -1)))
      }
      293 -> {
         out = _state_pipe(out, int(state_get_reg(out, "rdi", 0)))
      }
      60, 231 -> {
         out = out.set("deadended", true).set("exit_code", int(state_get_reg(out, "rdi", 0)))
      }
      332 -> {
         out = _state_statx_path(out, _read_cstring(out, int(state_get_reg(out, "rsi", 0)), 4096),
            int(state_get_reg(out, "r8", 0)))
      }
      _ -> {
         out = state_set_reg(out, "rax", -38).set("syscall_unsupported", nr)
      }
   }
   out.set("last_syscall", nr)
}

fn _linux_generic_syscall_name(int nr) str {
   match nr {
      17 -> "getcwd"
      23 -> "dup"
      24 -> "dup3"
      25 -> "fcntl"
      29 -> "ioctl"
      56 -> "openat"
      57 -> "close"
      62 -> "lseek"
      63 -> "read"
      64 -> "write"
      65 -> "readv"
      66 -> "writev"
      78 -> "readlinkat"
      80 -> "fstat"
      93 -> "exit"
      94 -> "exit_group"
      129 -> "kill"
      160 -> "uname"
      169 -> "gettimeofday"
      172 -> "getpid"
      173 -> "getppid"
      174 -> "getuid"
      175 -> "geteuid"
      176 -> "getgid"
      177 -> "getegid"
      214 -> "brk"
      222 -> "mmap"
      226 -> "mprotect"
      227 -> "munmap"
      278 -> "getrandom"
      291 -> "statx"
      _ -> "syscall_" + to_str(nr)
   }
}

fn _linux_arm_syscall_name(int nr) str {
   match nr {
      1 -> "exit"
      3 -> "read"
      4 -> "write"
      5 -> "open"
      6 -> "close"
      19 -> "lseek"
      20 -> "getpid"
      33 -> "access"
      37 -> "kill"
      45 -> "brk"
      54 -> "ioctl"
      78 -> "gettimeofday"
      90 -> "mmap"
      91 -> "munmap"
      122 -> "uname"
      145 -> "readv"
      146 -> "writev"
      192 -> "mmap2"
      197 -> "fstat64"
      248 -> "exit_group"
      295 -> "openat"
      322 -> "dup3"
      327 -> "readlinkat"
      345 -> "getrandom"
      397 -> "statx"
      _ -> "syscall_" + to_str(nr)
   }
}

fn _linux_syscall_name_for(str family, int nr) str {
   if family == "arm" { return _linux_arm_syscall_name(nr) }
   if family == "aarch64" || family == "riscv" { return _linux_generic_syscall_name(nr) }
   "syscall_" + to_str(nr)
}

fn _apply_linux_profile_syscall(dict st, dict prof) dict {
   def nr = int(_lift_get_reg(st, prof.get("nr", "rax"), 0))
   def family = prof.get("family", "")
   def name = _linux_syscall_name_for(family, nr)
   mut out = st
   match name {
      "read" -> {
         def fd = int(_linux_sys_arg(out, prof, 0, -1))
         def dst = int(_linux_sys_arg(out, prof, 1, 0))
         def n = int(_linux_sys_arg(out, prof, 2, 0))
         if dst > 0 && n > 0 {
            out = _state_read_fd(out, fd, dst, n)
            out = _linux_sys_ret(out, prof, int(out.get("last_io_count", 0)))
         } else {
            out = _linux_sys_ret(out, prof, 0)
         }
      }
      "write" -> {
         def fd = int(_linux_sys_arg(out, prof, 0, -1))
         def ptr = int(_linux_sys_arg(out, prof, 1, 0))
         def n = int(_linux_sys_arg(out, prof, 2, 0))
         def s = mem_read_bytes(out, ptr, n)
         if fd == 1 { out = state_append_stdout(out, s) }
         if fd == 2 { out = state_append_stderr(out, s) }
         out = _linux_sys_ret(out, prof, s.len)
      }
      "open" -> {
         def opened = _state_open_file(out, _read_cstring(out, int(_linux_sys_arg(out, prof, 0, 0)), 4096))
         out = _linux_sys_ret(opened, prof, int(opened.get("last_fd", -2)))
      }
      "openat" -> {
         def opened = _state_open_file(out, _read_cstring(out, int(_linux_sys_arg(out, prof, 1, 0)), 4096))
         out = _linux_sys_ret(opened, prof, int(opened.get("last_fd", -2)))
      }
      "close" -> {
         out = _linux_sys_ret(_fd_drop(out, int(_linux_sys_arg(out, prof, 0, -1))), prof, 0)
      }
      "lseek" -> {
         out = _state_seek_fd(out, int(_linux_sys_arg(out, prof, 0, -1)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)))
      }
      "mmap", "mmap2" -> {
         out = _state_mmap(out, int(_linux_sys_arg(out, prof, 0, 0)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)),
            int(_linux_sys_arg(out, prof, 3, 0)), int(_linux_sys_arg(out, prof, 4, -1)),
            int(_linux_sys_arg(out, prof, 5, 0)))
      }
      "mprotect" -> {
         out = _state_mprotect(out, int(_linux_sys_arg(out, prof, 0, 0)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)))
      }
      "munmap" -> {
         out = _state_munmap(out, int(_linux_sys_arg(out, prof, 0, 0)), int(_linux_sys_arg(out, prof, 1, 0)))
      }
      "brk" -> {
         out = _state_brk(out, int(_linux_sys_arg(out, prof, 0, 0)))
      }
      "access", "faccessat" -> {
         def pidx = name == "faccessat" ? 1 : 0
         out = _state_access_path(out, _read_cstring(out, int(_linux_sys_arg(out, prof, pidx, 0)), 4096))
      }
      "fstat", "fstat64" -> {
         out = _state_fstat_fd(out, int(_linux_sys_arg(out, prof, 0, -1)), int(_linux_sys_arg(out, prof, 1, 0)))
      }
      "readlinkat" -> {
         out = _state_readlink_path(out, _read_cstring(out, int(_linux_sys_arg(out, prof, 1, 0)), 4096),
            int(_linux_sys_arg(out, prof, 2, 0)), int(_linux_sys_arg(out, prof, 3, 0)))
      }
      "uname" -> {
         out = _state_uname(out, int(_linux_sys_arg(out, prof, 0, 0)))
      }
      "gettimeofday" -> {
         out = _state_gettimeofday(out, int(_linux_sys_arg(out, prof, 0, 0)), int(_linux_sys_arg(out, prof, 1, 0)))
      }
      "getpid" -> {
         out = _linux_sys_ret(out, prof, int(out.get("pid", 1337)))
      }
      "getppid" -> {
         out = _linux_sys_ret(out, prof, int(out.get("ppid", 1)))
      }
      "getuid", "geteuid" -> {
         out = _linux_sys_ret(out, prof, int(out.get("uid", 1000)))
      }
      "getgid", "getegid" -> {
         out = _linux_sys_ret(out, prof, int(out.get("gid", 1000)))
      }
      "getrandom" -> {
         out = _state_getrandom(out, int(_linux_sys_arg(out, prof, 0, 0)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)))
      }
      "readv" -> {
         out = _state_readv_fd(out, int(_linux_sys_arg(out, prof, 0, -1)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)))
      }
      "writev" -> {
         out = _state_writev_fd(out, int(_linux_sys_arg(out, prof, 0, -1)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)))
      }
      "dup" -> {
         out = _state_dup_fd(out, int(_linux_sys_arg(out, prof, 0, -1)))
      }
      "dup3" -> {
         out = _state_dup_fd(out, int(_linux_sys_arg(out, prof, 0, -1)), int(_linux_sys_arg(out, prof, 1, -1)))
      }
      "fcntl" -> {
         out = _state_fcntl(out, int(_linux_sys_arg(out, prof, 0, -1)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)))
      }
      "ioctl" -> {
         out = _state_ioctl(out, int(_linux_sys_arg(out, prof, 0, -1)),
            int(_linux_sys_arg(out, prof, 1, 0)), int(_linux_sys_arg(out, prof, 2, 0)))
      }
      "kill" -> {
         out = _state_signal_result(out, "kill", int(_linux_sys_arg(out, prof, 0, 0)), int(_linux_sys_arg(out, prof, 1, 0)))
      }
      "clock_gettime" -> {
         out = _state_clock_gettime(out, int(_linux_sys_arg(out, prof, 0, 0)), int(_linux_sys_arg(out, prof, 1, 0)))
      }
      "statx" -> {
         out = _state_statx_path(out, _read_cstring(out, int(_linux_sys_arg(out, prof, 1, 0)), 4096),
            int(_linux_sys_arg(out, prof, 4, 0)))
      }
      "exit", "exit_group" -> {
         out = out.set("deadended", true).set("exit_code", int(_linux_sys_arg(out, prof, 0, 0)))
      }
      _ -> {
         out = _linux_sys_ret(out, prof, -38).set("syscall_unsupported", nr)
      }
   }
   def ret_reg = prof.get("ret", "rax")
   if ret_reg != "rax" {
      def rv = _lift_get_reg(out, "rax", nil)
      if rv != nil { out = _linux_sys_ret(out, prof, rv) }
   }
   out.set("last_syscall", nr).set("last_syscall_name", name)
}

fn _apply_linux_syscall(dict st) dict {
   def prof = _linux_syscall_profile(st)
   if prof.get("family", "") == "x86_64" { return _apply_linux_x86_64_syscall(st).set("last_syscall_name", "linux_x86_64") }
   _apply_linux_profile_syscall(st, prof)
}

fn _x86_lite_successors(dict st) list {
   def proj = st.get("project", 0)
   def pc = state_addr(st)
   def loaded = _project_blob_at(proj, pc)
   def blob = loaded[0]
   def base = int(loaded[1])
   def off = pc - base
   def b0 = _byte_at(blob, off)
   if b0 < 0 { return [st.set("step_ok", false).set("step_reason", "pc_out_of_blob")] }
   if (b0 == 0x74 || b0 == 0x75) && _byte_at(blob, off + 1) >= 0 {
      def rel = _i8(_byte_at(blob, off + 1))
      def target = pc + 2 + rel
      def fall = pc + 2
      def flags = st.get("flags", dict())
      def zast = st.get("flag_ast", dict()).get("zf", 0)
      def zknown = flags.contains("zf") && !_is_ast(zast)
      def zf = flags.get("zf", false)
      def take = b0 == 0x74 ? zf : !zf
      if zknown {
         return [take ? _branch_state(st, pc, target, b0 == 0x74 ? "je" : "jne", true) : _branch_state(st, pc, fall, b0 == 0x74 ? "je" : "jne", false)]
      }
      return [_branch_state(st, pc, target, b0 == 0x74 ? "je" : "jne", true),
         _branch_state(st, pc, fall, b0 == 0x74 ? "je" : "jne", false)]
   }
   [_x86_lite_step(st)]
}

fn _x86_lite_step(dict st) dict {
   def proj = st.get("project", 0)
   def pc = state_addr(st)
   def loaded = _project_blob_at(proj, pc)
   def blob = loaded[0]
   def base = int(loaded[1])
   def off = pc - base
   def b0 = _byte_at(blob, off)
   if b0 < 0 { return st.set("step_ok", false).set("step_reason", "pc_out_of_blob") }
   mut next = pc + 1
   mut out = st
   mut desc = "db 0x" + to_hex(b0, 0)
   if b0 == 0x90 {
      desc = "nop"
   } elif b0 >= 0xb8 && b0 <= 0xbf && _byte_at(blob, off + 4) >= 0 {
      next = pc + 5
      def reg = _mov_imm_reg32(b0)
      def val = _u32le_any(blob, off + 1)
      out = state_set_reg(out, reg, val)
      out = state_set_reg(out, _reg32_to_64(reg), val)
      desc = "mov " + reg + ", " + to_str(val)
   } elif b0 == 0x48 && _byte_at(blob, off + 1) >= 0xb8 && _byte_at(blob, off + 1) <= 0xbf && _byte_at(blob, off + 9) >= 0 {
      next = pc + 10
      def reg64 = _mov_imm_reg64(_byte_at(blob, off + 1))
      def val64 = _u64le_any(blob, off + 2)
      out = state_set_reg(out, reg64, val64)
      desc = "mov " + reg64 + ", " + to_str(val64)
   } elif b0 == 0x31 && _byte_at(blob, off + 1) == 0xc0 {
      next = pc + 2
      out = state_set_reg(out, "rax", 0)
      out = state_set_reg(out, "eax", 0)
      out = _set_zf(out, true)
      desc = "xor eax, eax"
   } elif b0 == 0xff && _byte_at(blob, off + 1) == 0xc0 {
      next = pc + 2
      def v = int(state_get_reg(out, "rax", state_get_reg(out, "eax", 0))) + 1
      out = state_set_reg(out, "rax", v)
      out = state_set_reg(out, "eax", v & 0xffffffff)
      out = _set_zf(out, (v & 0xffffffff) == 0)
      desc = "inc eax"
   } elif b0 == 0x83 && _byte_at(blob, off + 1) == 0xf8 && _byte_at(blob, off + 2) >= 0 {
      next = pc + 3
      def imm = _i8(_byte_at(blob, off + 2))
      def eaxv = state_get_reg(out, "eax", state_get_reg(out, "rax", 0))
      if _is_ast(eaxv) {
         def sol = state_solver(out)
         out = _set_zf_ast(out, smt.mk_eq(sol.get("ctx", 0), eaxv, smt.bv_u32(sol.get("ctx", 0), imm & 0xffffffff)), "eax == " + to_str(imm))
      } else {
         def eax = int(eaxv) & 0xffffffff
         out = _set_zf_cmp(out, eax == (imm & 0xffffffff), "eax == " + to_str(imm))
      }
      desc = "cmp eax, " + to_str(imm)
   } elif b0 == 0x0f && (_byte_at(blob, off + 1) == 0xb6 || _byte_at(blob, off + 1) == 0xbe) && _base_reg_for_byte_mem(_byte_at(blob, off + 2)).len > 0 {
      def modrm = _byte_at(blob, off + 2)
      def has_disp = _byte_mem_has_disp(modrm)
      if !has_disp || _byte_at(blob, off + 3) >= 0 {
         next = pc + (has_disp ? 4 : 3)
         def disp = has_disp ? _i8(_byte_at(blob, off + 3)) : 0
         def mem_base = _base_reg_for_byte_mem(modrm)
         def ptr = _byte_mem_ptr(out, modrm, disp)
         def bv = mem_read(out, ptr, 0)
         def signed = _byte_at(blob, off + 1) == 0xbe
         out = _set_eax_from_byte(out, bv, signed)
         desc = (signed ? "movsx" : "movzx") + " eax, " + _describe_byte_mem(mem_base, disp)
      }
   } elif (b0 == 0x34 || b0 == 0x04 || b0 == 0x2c) && _byte_at(blob, off + 1) >= 0 {
      next = pc + 2
      def imm8 = _byte_at(blob, off + 1) & 255
      def op = b0 == 0x34 ? "xor" : (b0 == 0x04 ? "add" : "sub")
      out = _apply_al_imm8(out, op, imm8)
      desc = op + " al, " + to_str(imm8)
   } elif b0 == 0xa8 && _byte_at(blob, off + 1) >= 0 {
      next = pc + 2
      def imm8 = _byte_at(blob, off + 1) & 255
      out = _apply_al_bitmask_imm8(out, imm8)
      desc = "test al, " + to_str(imm8)
   } elif b0 == 0x85 && _byte_at(blob, off + 1) == 0xc0 {
      next = pc + 2
      out = _apply_eax_zero_check(out)
      desc = "test eax, eax"
   } elif b0 == 0xc6 && _base_reg_for_byte_mem(_byte_at(blob, off + 1)).len > 0 && _byte_at(blob, off + 2) >= 0 {
      def modrm = _byte_at(blob, off + 1)
      def has_disp = _byte_mem_has_disp(modrm)
      next = pc + (has_disp ? 4 : 3)
      def disp = has_disp ? _i8(_byte_at(blob, off + 2)) : 0
      def imm8 = _byte_at(blob, off + (has_disp ? 3 : 2)) & 255
      def mem_base = _base_reg_for_byte_mem(modrm)
      def ptr = _byte_mem_ptr(out, modrm, disp)
      out = mem_write_byte(out, ptr, imm8)
      desc = "mov " + _describe_byte_mem(mem_base, disp) + ", " + to_str(imm8)
   } elif b0 == 0x88 && _base_reg_for_byte_mem(_byte_at(blob, off + 1)).len > 0 {
      def modrm = _byte_at(blob, off + 1)
      def has_disp = _byte_mem_has_disp(modrm)
      if !has_disp || _byte_at(blob, off + 2) >= 0 {
         next = pc + (has_disp ? 3 : 2)
         def disp = has_disp ? _i8(_byte_at(blob, off + 2)) : 0
         def mem_base = _base_reg_for_byte_mem(modrm)
         def ptr = _byte_mem_ptr(out, modrm, disp)
         def reg_id = (modrm >> 3) & 7
         def value = _byte_reg_value(out, reg_id)
         out = mem_write_byte(out, ptr, _is_ast(value) ? value : _low8(value))
         desc = "mov " + _describe_byte_mem(mem_base, disp) + ", " + _byte_reg_name(reg_id)
      }
   } elif b0 == 0x3c && _byte_at(blob, off + 1) >= 0 {
      next = pc + 2
      def imm8 = _byte_at(blob, off + 1) & 255
      def alv = state_get_reg(out, "al", state_get_reg(out, "rax", state_get_reg(out, "eax", 0)))
      if _is_ast(alv) {
         def sol = state_solver(out)
         out = _set_zf_ast(out, smt.mk_eq(sol.get("ctx", 0), alv, smt.bv_u8(sol.get("ctx", 0), imm8)), "al == " + to_str(imm8))
      } else {
         out = _set_zf_cmp(out, _low8(alv) == imm8, "al == " + to_str(imm8))
      }
      desc = "cmp al, " + to_str(imm8)
   } elif b0 == 0x80 && _base_reg_for_byte_mem(_byte_at(blob, off + 1)).len > 0 && _byte_at(blob, off + 2) >= 0 {
      def modrm = _byte_at(blob, off + 1)
      def has_disp = _byte_mem_has_disp(modrm)
      next = pc + (has_disp ? 4 : 3)
      def disp = has_disp ? _i8(_byte_at(blob, off + 2)) : 0
      def imm8 = _byte_at(blob, off + (has_disp ? 3 : 2)) & 255
      def mem_base = _base_reg_for_byte_mem(modrm)
      def ptr = _byte_mem_ptr(out, modrm, disp)
      def bv = mem_read(out, ptr, -1)
      if _is_ast(bv) {
         def sol = state_solver(out)
         out = _set_zf_ast(out, smt.mk_eq(sol.get("ctx", 0), bv, smt.bv_u8(sol.get("ctx", 0), imm8)), "byte[" + to_hex(ptr, 0) + "] == " + to_str(imm8))
      } else {
         out = _set_zf_cmp(out, is_int(bv) && (int(bv) & 255) == imm8, "byte[" + to_hex(ptr, 0) + "] == " + to_str(imm8))
      }
      desc = "cmp " + _describe_byte_mem(mem_base, disp) + ", " + to_str(imm8)
   } elif b0 == 0xe8 && _byte_at(blob, off + 4) >= 0 {
      next = pc + 5
      def target = next + _i32le(blob, off + 1)
      def hook = _project_hook_at(proj, target)
      if hook.len > 0 {
         return _apply_hook(out, hook.set("ret", next))
      }
      out = _call_push(out, next)
      next = target
      desc = "call " + "0x" + to_hex(target, 0)
   } elif b0 == 0x0f && _byte_at(blob, off + 1) == 0x05 {
      next = pc + 2
      out = _apply_linux_syscall(out)
      desc = "syscall " + to_str(out.get("last_syscall", -1))
   } elif b0 == 0xc3 {
      desc = "ret"
      def popped = _call_pop(out)
      if popped.get("ok", false) {
         out = popped.get("state", out)
         next = int(popped.get("ret", pc))
      } else {
         next = pc
         out = out.set("deadended", true)
      }
   }
   out = _state_set_pc(out, next)
   _history_append(out.set("step_ok", true).set("step_reason", "ok"), {"addr": pc, "next": next, "backend": "lite_x86", "insn": desc})
}

fn _concrete_regs(dict st) dict {
   def regs = st.get("regs", dict())
   mut out = dict()
   def names = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip"]
   mut i = 0
   while i < names.len {
      def n = names[i]
      if regs.contains(n) && is_int(regs.get(n, 0)) { out = out.set(n, regs.get(n, 0)) }
      i += 1
   }
   out
}

fn state_step(dict st, int count=1, any opts=dict()) dict {
   "Step one state. Uses Unicorn for concrete x86_64 blob states when available,
   then falls back to a small built-in x86 shellcode stepper for deterministic
   local analysis and no-Unicorn installs."
   def proj = st.get("project", 0)
   def loaded = _project_blob_at(proj, state_addr(st))
   def blob = loaded[0]
   if _data_len(blob) <= 0 { return st.set("step_ok", false).set("step_reason", "no_blob") }
   def hook = _project_hook_at(proj, state_addr(st))
   if hook.len > 0 { return _apply_hook(st, hook) }
   if opts.get("backend", "auto") != "lite" && unicorn_available() && _project_arch(proj) == "x86" {
      def base = int(loaded[1])
      def regs = _concrete_regs(_state_set_pc(st, state_addr(st)))
      def run = unicorn_run_x86_64(blob, regs, base, count)
      if run.get("ok", false) {
         mut out = st
         def r = run.get("regs", dict())
         def ks = r.keys()
         mut i = 0
         while i < ks.len {
            out = state_set_reg(out, ks[i], r.get(ks[i], 0))
            i += 1
         }
         out = _state_set_pc(out, int(r.get("rip", state_addr(st))))
         return _history_append(out.set("step_ok", true).set("step_reason", "ok"), {"addr": state_addr(st), "next": out.get("addr", 0), "backend": "unicorn", "count": count})
      }
   }
   mut cur = st
   mut i = 0
   while i < max(1, count) && !cur.get("deadended", false) {
      cur = _x86_lite_step(cur)
      if !cur.get("step_ok", false) { break }
      i += 1
   }
   cur
}

fn state_successors(dict st, int count=1, any opts=dict()) list {
   "Return successor states for one path. Branch-capable backends may return
   more than one state, which is the primitive `simgr_step` uses for path
   exploration."
   mut states = [st]
   mut i = 0
   while i < max(1, count) {
      mut next = []
      mut si = 0
      while si < states.len {
         def cur = states[si]
         def hook = _project_hook_at(cur.get("project", 0), state_addr(cur))
         if cur.get("deadended", false) || !cur.get("step_ok", true) {
            next = next.append(cur)
         } elif hook.len > 0 {
            next = next.append(_apply_hook(cur, hook))
         } elif opts.get("backend", "auto") == "lite" {
            def succ = _x86_lite_successors(cur)
            mut sj = 0
            while sj < succ.len { next = next.append(succ[sj]) sj += 1 }
         } else {
            next = next.append(state_step(cur, 1, opts))
         }
         si += 1
      }
      states = next
      i += 1
   }
   states
}

fn state_summary(dict st) dict {
   "Return a compact state summary for UI/logging."
   {"addr": st.get("addr", 0), "regs": st.get("regs", dict()).keys(), "mem": st.get("mem", dict()).keys(),
      "constraints": st.get("constraints", []).len, "symbolics": st.get("symbolics", dict()).keys(),
      "path_conditions": st.get("path_conditions", []).len,
      "history": st.get("history", []).len, "deadended": st.get("deadended", false)}
}

fn state_snapshot(dict st, any opts=dict()) dict {
   "Return a stable state snapshot for UI panes, logs, and before/after diffs."
   def include_mem = is_dict(opts) ? opts.get("mem", true) : true
   {"kind": "state_snapshot", "addr": state_addr(st), "regs": _dict_copy(state_regs(st)),
      "mem": include_mem ? _dict_copy(state_mem(st)) : dict(), "mem_keys": state_mem(st).keys(),
      "constraints": state_constraints(st).len, "path_conditions": st.get("path_conditions", []),
      "symbolics": state_symbolics(st).keys(), "stdin": state_stdin(st), "argv": state_argv(st),
      "stdout": state_stdout(st), "stderr": state_stderr(st), "output": state_output(st),
      "history": st.get("history", []).len, "deadended": st.get("deadended", false),
      "step_ok": st.get("step_ok", true), "step_reason": st.get("step_reason", "")}
}

fn _snap(dict x) dict {
   x.get("kind", "") == "state_snapshot" ? x : state_snapshot(x)
}

fn _key_union(dict a, dict b) list {
   mut out = []
   def ak = a.keys()
   mut i = 0
   while i < ak.len { out = _append_unique(out, ak[i]) i += 1 }
   def bk = b.keys()
   i = 0
   while i < bk.len { out = _append_unique(out, bk[i]) i += 1 }
   out
}

fn _dict_value_changes(dict before, dict after, str label) list {
   def keys = _key_union(before, after)
   mut out = []
   mut i = 0
   while i < keys.len {
      def k = keys[i]
      def had = before.contains(k)
      def has = after.contains(k)
      def bv = before.get(k, nil)
      def av = after.get(k, nil)
      if !had && has { out = out.append({"kind": "added", "space": label, "key": k, "after": av}) }
      elif had && !has { out = out.append({"kind": "removed", "space": label, "key": k, "before": bv}) }
      elif to_str(bv) != to_str(av) { out = out.append({"kind": "changed", "space": label, "key": k, "before": bv, "after": av}) }
      i += 1
   }
   out
}

fn _str_delta(str before, str after) str {
   str.startswith(after, before) ? slice(after, before.len, after.len, 1) : after
}

fn state_diff(dict before, dict after, any opts=dict()) dict {
   "Return changed registers, memory entries, outputs, constraints, and control state."
   def a = _snap(before)
   def b = _snap(after)
   def include_mem = is_dict(opts) ? opts.get("mem", true) : true
   def regs = _dict_value_changes(a.get("regs", dict()), b.get("regs", dict()), "reg")
   def mem = include_mem ? _dict_value_changes(a.get("mem", dict()), b.get("mem", dict()), "mem") : []
   mut control = dict()
   if a.get("addr", 0) != b.get("addr", 0) { control = control.set("addr", {"before": a.get("addr", 0), "after": b.get("addr", 0)}) }
   if a.get("deadended", false) != b.get("deadended", false) { control = control.set("deadended", {"before": a.get("deadended", false), "after": b.get("deadended", false)}) }
   {"kind": "state_diff", "addr": {"before": a.get("addr", 0), "after": b.get("addr", 0)},
      "control": control, "regs": regs, "mem": mem,
      "stdout": {"before": a.get("stdout", ""), "after": b.get("stdout", ""), "delta": _str_delta(a.get("stdout", ""), b.get("stdout", ""))},
      "stderr": {"before": a.get("stderr", ""), "after": b.get("stderr", ""), "delta": _str_delta(a.get("stderr", ""), b.get("stderr", ""))},
      "constraints": {"before": a.get("constraints", 0), "after": b.get("constraints", 0), "delta": int(b.get("constraints", 0)) - int(a.get("constraints", 0))},
      "path_conditions": {"before": a.get("path_conditions", []).len, "after": b.get("path_conditions", []).len},
      "history": {"before": a.get("history", 0), "after": b.get("history", 0), "delta": int(b.get("history", 0)) - int(a.get("history", 0))},
      "changed": regs.len + mem.len + control.keys().len}
}

fn watch_addr() dict {
   "Create a watch predicate that matches an instruction-address change."
   {"kind": "watch", "pred": "addr_changed"}
}

fn watch_reg(any reg="") dict {
   "Create a watch predicate that matches any register change, or one named register."
   {"kind": "watch", "pred": "reg_changed", "reg": to_str(reg)}
}

fn watch_mem(any addr=0) dict {
   "Create a watch predicate that matches any memory change, or one address."
   {"kind": "watch", "pred": "mem_changed", "addr": int(addr)}
}

fn watch_stdout(any text="") dict {
   "Create a watch predicate that matches stdout growth, optionally containing `text`."
   {"kind": "watch", "pred": "stdout_delta", "text": to_str(text)}
}

fn watch_stderr(any text="") dict {
   "Create a watch predicate that matches stderr growth, optionally containing `text`."
   {"kind": "watch", "pred": "stderr_delta", "text": to_str(text)}
}

fn watch_output(any text="") dict {
   "Create a watch predicate that matches stdout or stderr growth, optionally containing `text`."
   {"kind": "watch", "pred": "output_delta", "text": to_str(text)}
}

fn _change_has_key(list changes, any key) bool {
   mut i = 0
   while i < changes.len {
      if changes[i].get("key", nil) == key || to_str(changes[i].get("key", "")) == to_str(key) { return true }
      i += 1
   }
   false
}

fn _delta_matches(dict section, str text) bool {
   def d = section.get("delta", "")
   d.len > 0 && (text.len == 0 || contains(d, text))
}

fn state_diff_matches(dict diff, any pred) bool {
   "Return true when a state diff matches a watch predicate."
   if !is_dict(pred) { return false }
   def p = pred.get("pred", "")
   if p == "addr_changed" { return diff.get("control", dict()).contains("addr") }
   if p == "reg_changed" {
      def r = pred.get("reg", "")
      return r.len == 0 ? diff.get("regs", []).len > 0 : _change_has_key(diff.get("regs", []), r)
   }
   if p == "mem_changed" {
      def a = int(pred.get("addr", 0))
      return a == 0 ? diff.get("mem", []).len > 0 : _change_has_key(diff.get("mem", []), a)
   }
   if p == "stdout_delta" { return _delta_matches(diff.get("stdout", dict()), pred.get("text", "")) }
   if p == "stderr_delta" { return _delta_matches(diff.get("stderr", dict()), pred.get("text", "")) }
   if p == "output_delta" {
      def text = pred.get("text", "")
      return _delta_matches(diff.get("stdout", dict()), text) || _delta_matches(diff.get("stderr", dict()), text)
   }
   false
}

fn _solution_reg_record(dict st, str reg) dict {
   def sy = st.get("symbolics", dict())
   def aliases = _lift_reg_aliases(reg)
   mut i = 0
   while i < aliases.len {
      def rec = sy.get(aliases[i], dict())
      if is_dict(rec) && rec.get("kind", "") == "reg" { return rec }
      i += 1
   }
   dict()
}

fn _solution_reg_u64(dict st, str reg) any {
   def rec = _solution_reg_record(st, reg)
   if rec.len > 0 && _is_ast(rec.get("ast", 0)) {
      return eval_u64(state_solver(st), rec.get("ast", 0))
   }
   state_eval_reg_u64(st, reg)
}

fn _solution_reg_hex(dict st, str reg) any {
   def rec = _solution_reg_record(st, reg)
   if rec.len > 0 && _is_ast(rec.get("ast", 0)) {
      return eval_hex(state_solver(st), rec.get("ast", 0), int(rec.get("bits", _reg_bits(st, reg))))
   }
   state_eval_reg_hex(st, reg)
}

fn _solution_regs(dict st, list regs) dict {
   mut out = dict()
   mut i = 0
   while i < regs.len {
      def r = regs[i]
      def v = _solution_reg_u64(st, r)
      if v != nil { out = out.set(r, v) }
      i += 1
   }
   out
}

fn _solution_argv(dict st) list {
   def args = state_argv(st)
   mut out = []
   mut i = 0
   while i < args.len {
      out = out.append(state_eval_argv_ascii(st, i))
      i += 1
   }
   out
}

fn _solution_ascii_bytes(dict st, list xs) str {
   mut b = str.Builder(xs.len)
   mut i = 0
   while i < xs.len {
      def v = eval_u64(state_solver(st), xs[i])
      b = str.builder_append_byte(b, (v == nil ? 0 : int(v)) & 255)
      i += 1
   }
   def s = str.builder_to_str(b)
   str.builder_free(b)
   s
}

fn _solution_stdin(dict st) any {
   def nm = st.get("stdin_symbolic", "")
   if nm.len > 0 {
      def xs = st.get("symbolics", dict()).get(nm, [])
      if is_list(xs) { return _solution_ascii_bytes(st, xs) }
   }
   state_eval_stdin_ascii(st)
}

fn _solution_symbolics(dict st) dict {
   def sy = state_symbolics(st)
   mut out = dict()
   def ks = sy.keys()
   mut i = 0
   while i < ks.len {
      def k = ks[i]
      def v = sy.get(k, nil)
      if is_dict(v) {
         def kind = v.get("kind", "")
         if kind == "reg" {
            mut solved = _solution_reg_u64(st, k)
            if solved == nil { solved = _solution_reg_hex(st, k) }
            out = out.set(k, solved)
            def public_name = v.get("name", "")
            if public_name.len > 0 && public_name != k { out = out.set(public_name, solved) }
         } elif kind == "mem" {
            def ascii = state_eval_mem_ascii(st, v.get("addr", 0))
            out = out.set(k, is_str(ascii) ? ascii : state_eval_mem_bytes(st, v.get("addr", 0)))
         }
      } elif is_list(v) {
         out = out.set(k, _solution_ascii_bytes(st, v))
      }
      i += 1
   }
   out
}

fn _solution_trace(dict st, int limit=64) list {
   def hist = st.get("history", [])
   mut out = []
   mut i = 0
   while i < hist.len && (limit <= 0 || i < limit) {
      def ev = hist[i]
      if is_dict(ev) {
         mut row = dict()
         if ev.contains("addr") {
            row = row.set("addr", int(ev.get("addr", 0))).set("addr_hex", "0x" + to_hex(int(ev.get("addr", 0)), 0))
         }
         if ev.contains("next") {
            row = row.set("next", int(ev.get("next", 0))).set("next_hex", "0x" + to_hex(int(ev.get("next", 0)), 0))
         }
         if ev.contains("backend") { row = row.set("backend", ev.get("backend", "")) }
         if ev.contains("insn") { row = row.set("insn", ev.get("insn", "")) }
         if ev.contains("hook") { row = row.set("hook", ev.get("hook", "")) }
         if ev.contains("condition") { row = row.set("condition", ev.get("condition", "")) }
         if ev.contains("branch") { row = row.set("branch", ev.get("branch", false)) }
         out = out.append(row)
      }
      i += 1
   }
   out
}

fn state_history(dict st) list {
   "Return the raw execution history records attached to `st`."
   _list_copy(st.get("history", []))
}

fn state_trace(dict st, int limit=64) list {
   "Return normalized execution trace records with hex addresses.
   This is the direct path-inspection API for scripts that do not need a full
   solved model."
   _solution_trace(st, limit)
}

fn state_path(dict st, int limit=0) list {
   "Return the address path followed by `st`, including the current address."
   def hist = st.get("history", [])
   mut out = []
   mut i = 0
   while i < hist.len && (limit <= 0 || out.len < limit) {
      def ev = hist[i]
      if is_dict(ev) && ev.contains("addr") && out.len == 0 { out = out.append(int(ev.get("addr", 0))) }
      if is_dict(ev) && ev.contains("next") && (limit <= 0 || out.len < limit) { out = out.append(int(ev.get("next", 0))) }
      i += 1
   }
   if out.len == 0 { out = [state_addr(st)] }
   elif (limit <= 0 || out.len < limit) && out[out.len - 1] != state_addr(st) { out = out.append(state_addr(st)) }
   out
}

fn state_path_text(dict st, int limit=0) str {
   "Return a compact `0x... -> 0x...` path string for UI/log output."
   def p = state_path(st, limit)
   mut out = ""
   mut i = 0
   while i < p.len {
      if i > 0 { out += " -> " }
      out += "0x" + to_hex(int(p[i]), 0)
      i += 1
   }
   out
}

fn simgr_traces(dict manager, str stash="found", int limit=64) list {
   "Return normalized traces for every state in a simulation-manager stash."
   def states = manager.get(stash, [])
   mut out = []
   mut i = 0
   while i < states.len {
      out = out.append({"stash": stash, "index": i, "addr": state_addr(states[i]),
            "path": state_path(states[i], limit), "path_text": state_path_text(states[i], limit),
            "trace": state_trace(states[i], limit)})
      i += 1
   }
   out
}

fn state_solution(dict st, any opts=dict()) dict {
   "Return a solved, analyst-facing model for one state.
   This is the equivalent of pulling stdin/argv/register models from an angr
   found state after exploration."
   def regs = is_dict(opts) ? opts.get("regs", ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rip"]) : ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rip"]
   def trace_limit = is_dict(opts) ? int(opts.get("trace_limit", 64)) : 64
   {"addr": state_addr(st), "sat": state_feasible(st), "stdin": _solution_stdin(st),
      "argv": _solution_argv(st), "env": state_env(st), "regs": _solution_regs(st, regs),
      "symbolics": _solution_symbolics(st), "stdout": state_stdout(st), "stderr": state_stderr(st),
      "output": state_output(st), "constraints": st.get("constraints", []).len,
      "constraint_notes": st.get("constraint_notes", []),
      "path_conditions": st.get("path_conditions", []), "trace": _solution_trace(st, trace_limit),
      "deadended": st.get("deadended", false)}
}

fn _solution_atom_allowed(str name, dict opts) bool {
   if !is_dict(opts) || !opts.contains("symbols") { return true }
   def keep = opts.get("symbols", [])
   if !is_list(keep) || keep.len == 0 { return true }
   keep.contains(name)
}

fn _solution_atoms(dict st, any opts=dict()) list {
   def sy = state_symbolics(st)
   def ks = sy.keys()
   mut out = []
   mut i = 0
   while i < ks.len {
      def k = ks[i]
      def v = sy.get(k, nil)
      if _solution_atom_allowed(k, opts) {
         if is_dict(v) && v.get("kind", "") == "reg" {
            out = out.append({"name": k, "ast": v.get("ast", 0), "bits": int(v.get("bits", 64))})
         } elif is_dict(v) && v.get("kind", "") == "mem" {
            def xs = v.get("bytes", [])
            mut j = 0
            while j < xs.len {
               out = out.append({"name": k, "index": j, "ast": xs[j], "bits": 8})
               j += 1
            }
         } elif is_list(v) {
            mut j = 0
            while j < v.len {
               out = out.append({"name": k, "index": j, "ast": v[j], "bits": 8})
               j += 1
            }
         }
      }
      i += 1
   }
   out
}

fn _state_solution_atoms(dict st, any opts=dict()) list {
   _solution_atoms(st, opts)
}

fn _block_solution_atoms(dict st, list atoms) int {
   if atoms.len == 0 { return 0 }
   def sol = state_solver(st)
   if !sol.get("ok", false) { return 0 }
   def ctx = sol.get("ctx", 0)
   mut clauses = []
   mut i = 0
   while i < atoms.len {
      def a = atoms[i]
      def ast = a.get("ast", 0)
      def bits = int(a.get("bits", 0))
      def h = eval_hex(sol, ast, bits)
      if h != nil && bits > 0 {
         clauses = clauses.append(smt.mk_neq(ctx, ast, smt.bv_hex(ctx, h, bits)))
      }
      i += 1
   }
   if clauses.len == 0 { return 0 }
   solver_add(sol, clauses.len == 1 ? clauses[0] : smt.mk_or(ctx, clauses))
   clauses.len
}

fn state_solutions(dict st, int limit=8, any opts=dict()) list {
   "Enumerate up to `limit` solved models for one feasible state by blocking
   the full symbolic tuple after each model. This exposes multiple satisfying
   stdin/argv/register/memory assignments on a single found path."
   mut out = []
   def atoms = _state_solution_atoms(st, opts)
   mut i = 0
   mut left = limit <= 0 ? 1 : limit
   while i < left && state_feasible(st) {
      out = out.append(state_solution(st, opts).set("ok", true).set("model_index", i))
      if _block_solution_atoms(st, atoms) == 0 { break }
      i += 1
   }
   out
}

fn solver(any logic="QF_BV") dict {
   "Create a symbolic solver record backed by Z3 when available."
   if !z3_available() { return {"ok": false, "ctx": 0, "solver": 0, "logic": logic, "reason": "z3_unavailable"} }
   def ctx = smt.ctx_new()
   if !ctx { return {"ok": false, "ctx": 0, "solver": 0, "logic": logic, "reason": "ctx_failed"} }
   def s = smt.solver_new_for_logic(ctx, to_str(logic))
   if !s {
      smt.ctx_del(ctx)
      return {"ok": false, "ctx": 0, "solver": 0, "logic": logic, "reason": "solver_failed"}
   }
   {"ok": true, "ctx": ctx, "solver": s, "logic": logic, "vars": dict(16)}
}

fn solver_free(dict sol) any {
   "Release a solver record."
   if !sol.get("ok", false) { return nil }
   smt.solver_del(sol.get("ctx", 0), sol.get("solver", 0))
   if !sol.get("borrow_ctx", false) { smt.ctx_del(sol.get("ctx", 0)) }
}

fn solver_add(dict sol, any ast) dict {
   "Assert a Z3 AST into a solver record."
   if sol.get("ok", false) { smt.solver_assert(sol.get("ctx", 0), sol.get("solver", 0), ast) }
   sol
}

fn solver_check(dict sol) int {
   "Return SAT/UNSAT/UNKNOWN for a solver record."
   if !sol.get("ok", false) { return UNKNOWN }
   def r = smt.solver_check_result(sol.get("ctx", 0), sol.get("solver", 0))
   r
}

fn solver_sat(dict sol) bool { solver_check(sol) == SAT }

fn solver_model(dict sol, dict vars) dict {
   "Evaluate a map of name -> {ast,bits} records as hex strings."
   mut out = dict(vars.len)
   def ks = vars.keys()
   mut i = 0
   while i < ks.len {
      def k = ks[i]
      def v = vars.get(k, 0)
      if is_dict(v) { out = out.set(k, eval_hex(sol, v.get("ast", 0), v.get("bits", 0))) }
      i += 1
   }
   out
}

fn solver_block_hex(dict sol, any ast, str hex, int bits) dict {
   "Add a blocking constraint excluding a previous hex model value."
   if sol.get("ok", false) { smt.solver_assert_decl_not_hex(sol.get("ctx", 0), sol.get("solver", 0), ast, hex, bits) }
   sol
}

fn enumerate_hex(dict sol, any ast, int bits, int limit=8) list {
   "Enumerate up to `limit` model values for one bitvector AST."
   mut out = []
   mut i = 0
   while i < limit && solver_sat(sol) {
      def h = eval_hex(sol, ast, bits)
      if h == nil { break }
      out = out.append(h)
      solver_block_hex(sol, ast, h, bits)
      i += 1
   }
   out
}

fn bvs(dict sol, str name, int bits) any {
   "Create a symbolic bitvector variable in a solver record."
   if !sol.get("ok", false) { return 0 }
   def ast = smt.bv_const(sol.get("ctx", 0), name, bits)
   sol.set("vars", sol.get("vars", dict()).set(name, {"ast": ast, "bits": bits, "kind": "bv"}))
   ast
}

fn bvv(dict sol, any value, int bits) any {
   "Create a concrete bitvector value in a solver record."
   if !sol.get("ok", false) { return 0 }
   def v = int(value)
   if bits > 32 && v >= 0 {
      return smt.bv_hex(sol.get("ctx", 0), smt.hex_width(to_hex(v, 0), bits), bits)
   }
   smt.bv_u64(sol.get("ctx", 0), v, bits)
}

fn bytes_symbolic(dict sol, str prefix, int n) list {
   "Create symbolic bytes named prefix0..prefixN."
   if !sol.get("ok", false) { return [] }
   def xs = smt.bv_bytes(sol.get("ctx", 0), prefix, n)
   sol.set("vars", sol.get("vars", dict()).set(prefix, {"ast": xs, "bits": 8, "count": n, "kind": "bytes"}))
   xs
}

fn byte_constraints_ascii(dict sol, list xs, int lo=32, int hi=126) int {
   "Constrain symbolic bytes to an ASCII range."
   if !sol.get("ok", false) { return 0 }
   smt.solver_assert_bytes_ascii_range(sol.get("ctx", 0), sol.get("solver", 0), xs, lo, hi)
}

fn byte_constraints_eq(dict sol, list xs, any values) int {
   "Constrain symbolic bytes to exact concrete bytes."
   if !sol.get("ok", false) { return 0 }
   smt.solver_assert_bytes_eq(sol.get("ctx", 0), sol.get("solver", 0), xs, values)
}

fn byte_constraints_xor_eq(dict sol, list xs, any key, any values) int {
   "Constrain `(xs[i] xor key[i]) == values[i]`."
   if !sol.get("ok", false) { return 0 }
   smt.solver_assert_bytes_xor_eq(sol.get("ctx", 0), sol.get("solver", 0), xs, key, values)
}

fn eval_u64(dict sol, any ast) any {
   "Evaluate a bitvector AST as u64 in the current model."
   if !sol.get("ok", false) { return nil }
   smt.model_eval_u64(sol.get("ctx", 0), sol.get("solver", 0), ast)
}

fn eval_hex(dict sol, any ast, int bits=0) any {
   "Evaluate a bitvector AST as hex."
   if !sol.get("ok", false) { return nil }
   if bits > 0 { return smt.model_eval_hex_width(sol.get("ctx", 0), sol.get("solver", 0), ast, bits) }
   smt.model_eval_hex(sol.get("ctx", 0), sol.get("solver", 0), ast)
}

fn eval_bytes(dict sol, list xs) any {
   "Evaluate symbolic bytes as integers."
   if !sol.get("ok", false) { return nil }
   smt.model_eval_bytes(sol.get("ctx", 0), sol.get("solver", 0), xs)
}

fn eval_ascii(dict sol, list xs) any {
   "Evaluate symbolic bytes as an ASCII string."
   if !sol.get("ok", false) { return nil }
   smt.model_eval_ascii(sol.get("ctx", 0), sol.get("solver", 0), xs)
}

fn solve_bytes_xor_eq(any key, any cipher, int n=0, int lo=32, int hi=126) dict {
   "Solve plaintext bytes where `(plain[i] xor key[i]) == cipher[i]`."
   mut count = n
   if count <= 0 {
      if is_str(cipher) || is_bytes(cipher) || is_list(cipher) { count = cipher.len }
   }
   def sol = solver()
   if !sol.get("ok", false) { return {"sat": false, "reason": sol.get("reason", "solver_unavailable")} }
   def xs = bytes_symbolic(sol, "b", count)
   byte_constraints_ascii(sol, xs, lo, hi)
   byte_constraints_xor_eq(sol, xs, key, cipher)
   def sat = solver_sat(sol)
   def out = sat ? eval_ascii(sol, xs) : nil
   def hex = sat ? eval_bytes(sol, xs) : nil
   solver_free(sol)
   return {"sat": sat, "ascii": out, "bytes": hex}
}

fn solve_ascii_sum8(int n, int sum8, int lo=32, int hi=126) dict {
   "Find `n` ASCII bytes whose byte-wise sum equals `sum8` modulo 256."
   def sol = solver()
   if !sol.get("ok", false) { return {"sat": false, "reason": sol.get("reason", "solver_unavailable")} }
   def xs = bytes_symbolic(sol, "b", n)
   byte_constraints_ascii(sol, xs, lo, hi)
   smt.solver_assert_bytes_add_sum8(sol.get("ctx", 0), sol.get("solver", 0), xs, sum8)
   def sat = solver_sat(sol)
   def out = sat ? eval_ascii(sol, xs) : nil
   solver_free(sol)
   return {"sat": sat, "ascii": out}
}

fn simgr(any st) dict {
   "Create an angr-style simulation manager record."
   {"active": [st], "found": [], "avoid": [], "deadended": [], "errored": [], "unsat": [], "watch": []}
}

fn simgr_stashes(dict manager) list {
   "Return known simulation-manager stash names."
   ["active", "found", "avoid", "deadended", "errored", "unsat", "watch"]
}

fn simgr_stash(dict manager, str name) list {
   "Return one simulation-manager stash."
   manager.get(name, [])
}

fn simgr_summary(dict manager) dict {
   "Return stash counts for UI/logging."
   mut out = dict()
   def names = simgr_stashes(manager)
   mut i = 0
   while i < names.len {
      def n = names[i]
      out = out.set(n, manager.get(n, []).len)
      i += 1
   }
   out.set("steps", int(manager.get("steps", 0)))
}

fn simgr_solution(dict manager, str stash="found", int index=0, any opts=dict()) dict {
   "Return a solved model for one state in a simulation-manager stash."
   def states = manager.get(stash, [])
   if index < 0 || index >= states.len { return {"ok": false, "reason": "missing_state", "stash": stash, "index": index} }
   state_solution(states[index], opts).set("ok", true).set("stash", stash).set("index", index)
}

fn simgr_solutions(dict manager, str stash="found", any opts=dict()) list {
   "Return solved models for every state in a simulation-manager stash."
   def states = manager.get(stash, [])
   mut out = []
   mut i = 0
   while i < states.len {
      out = out.append(state_solution(states[i], opts).set("ok", true).set("stash", stash).set("index", i))
      i += 1
   }
   out
}

fn simgr_solutions_all(dict manager, str stash="found", int limit=8, any opts=dict()) list {
   "Return enumerated solved models for every state in a simulation-manager stash."
   def states = manager.get(stash, [])
   mut out = []
   mut i = 0
   while i < states.len {
      def sols = state_solutions(states[i], limit, opts)
      mut j = 0
      while j < sols.len {
         out = out.append(sols[j].set("stash", stash).set("index", i))
         j += 1
      }
      i += 1
   }
   out
}

fn _stash_matches(dict st, any pred) bool {
   pred == 0 ? true : state_matches(st, pred)
}

fn simgr_move(dict manager, str src="active", str dst="found", any pred=0, int limit=0) dict {
   "Move matching states from one stash to another."
   mut remaining = []
   mut to = manager.get(dst, [])
   def states = manager.get(src, [])
   mut moved = 0
   mut i = 0
   while i < states.len {
      def st = states[i]
      if _stash_matches(st, pred) && (limit <= 0 || moved < limit) {
         to = to.append(st)
         moved += 1
      } else {
         remaining = remaining.append(st)
      }
      i += 1
   }
   manager.set(src, remaining).set(dst, to)
}

fn simgr_drop(dict manager, str stash="active", any pred=0, int limit=0) dict {
   "Drop matching states from a stash."
   mut kept = []
   def states = manager.get(stash, [])
   mut dropped = 0
   mut i = 0
   while i < states.len {
      def st = states[i]
      if _stash_matches(st, pred) && (limit <= 0 || dropped < limit) {
         dropped += 1
      } else {
         kept = kept.append(st)
      }
      i += 1
   }
   manager.set(stash, kept)
}

fn simgr_prune(dict manager, str stash="active") dict {
   "Move solver-proven infeasible states from a stash into `unsat`."
   mut active = []
   mut unsat = manager.get("unsat", [])
   def states = manager.get(stash, [])
   mut i = 0
   while i < states.len {
      def st = states[i]
      if !state_feasible(st) { unsat = unsat.append(st) }
      else { active = active.append(st) }
      i += 1
   }
   manager.set(stash, active).set("unsat", unsat)
}

fn simgr_step(dict manager, int count=1, any opts=dict()) dict {
   "Step every active state once and repartition active/deadended/errored paths."
   mut active = []
   mut dead = manager.get("deadended", [])
   mut err = manager.get("errored", [])
   mut unsat = manager.get("unsat", [])
   def prune_unsat = opts.get("prune_unsat", true)
   def states = manager.get("active", [])
   mut i = 0
   while i < states.len {
      def succ = state_successors(states[i], count, opts)
      mut j = 0
      while j < succ.len {
         def stepped = succ[j]
         if !stepped.get("step_ok", false) { err = err.append(stepped) }
         elif prune_unsat && !state_feasible(stepped) { unsat = unsat.append(stepped) }
         elif stepped.get("deadended", false) { dead = dead.append(stepped) }
         else { active = active.append(stepped) }
         j += 1
      }
      i += 1
   }
   manager.set("active", active).set("deadended", dead).set("errored", err).set("unsat", unsat).set("steps", int(manager.get("steps", 0)) + count)
}

fn simgr_lifted_step(dict manager, list rows, int count=1, any opts=dict()) dict {
   "Step active states through lifted decompiler rows instead of raw bytes."
   mut mgr = manager
   mut c = 0
   while c < max(1, count) {
      mut active = []
      mut dead = mgr.get("deadended", [])
      mut err = mgr.get("errored", [])
      mut unsat = mgr.get("unsat", [])
      def prune_unsat = opts.get("prune_unsat", true)
      def states = mgr.get("active", [])
      mut i = 0
      while i < states.len {
         def succ = _lifted_step_state(states[i], rows, opts)
         mut j = 0
         while j < succ.len {
            def stepped = succ[j]
            if !stepped.get("step_ok", false) { err = err.append(stepped) }
            elif prune_unsat && !state_feasible(stepped) { unsat = unsat.append(stepped) }
            elif stepped.get("deadended", false) { dead = dead.append(stepped) }
            else { active = active.append(stepped) }
            j += 1
         }
         i += 1
      }
      mgr = mgr.set("active", active).set("deadended", dead).set("errored", err).set("unsat", unsat).set("steps", int(mgr.get("steps", 0)) + 1)
      c += 1
   }
   mgr
}

fn _classify_watch_successor(dict after, list active, list dead, list err, list unsat, bool prune_unsat) list {
   if !after.get("step_ok", false) { return [active, dead, err.append(after), unsat] }
   if prune_unsat && !state_feasible(after) { return [active, dead, err, unsat.append(after)] }
   if after.get("deadended", false) { return [active, dead.append(after), err, unsat] }
   [active.append(after), dead, err, unsat]
}

fn watch_until(dict manager, any watch_pred, int steps=1, any opts=dict()) dict {
   "Step active states until a watch predicate matches a before/after diff.
   Returns the manager with a `watch` stash of records containing before, after,
   and diff."
   mut mgr = manager
   mut watches = mgr.get("watch", [])
   mut left = steps <= 0 ? 1 : steps
   def stop_on_watch = opts.get("stop_on_watch", true)
   def prune_unsat = opts.get("prune_unsat", true)
   while left > 0 {
      mut active = []
      mut dead = mgr.get("deadended", [])
      mut err = mgr.get("errored", [])
      mut unsat = mgr.get("unsat", [])
      def states = mgr.get("active", [])
      mut i = 0
      while i < states.len {
         def before = states[i]
         def before_snap = state_snapshot(before, opts.get("diff", dict()))
         def succ = state_successors(before, 1, opts)
         mut j = 0
         while j < succ.len {
            def after = succ[j]
            def after_snap = state_snapshot(after, opts.get("diff", dict()))
            def diff = state_diff(before_snap, after_snap, opts.get("diff", dict()))
            if state_diff_matches(diff, watch_pred) {
               watches = watches.append({"before": before_snap, "after": after_snap, "state": after, "diff": diff, "watch": watch_pred, "step": int(mgr.get("steps", 0)) + 1})
            }
            def parts = _classify_watch_successor(after, active, dead, err, unsat, prune_unsat)
            active = parts[0]
            dead = parts[1]
            err = parts[2]
            unsat = parts[3]
            j += 1
         }
         i += 1
      }
      mgr = mgr.set("active", active).set("deadended", dead).set("errored", err).set("unsat", unsat)
      .set("watch", watches).set("watch_count", watches.len).set("steps", int(mgr.get("steps", 0)) + 1)
      if active.len == 0 || (stop_on_watch && watches.len > 0) { break }
      left -= 1
   }
   mgr
}

fn watch_lifted_until(dict manager, list rows, any watch_pred, int steps=1, any opts=dict()) dict {
   "Step active states through lifted rows until a watch predicate matches."
   mut mgr = manager
   mut watches = mgr.get("watch", [])
   mut left = steps <= 0 ? 1 : steps
   def stop_on_watch = opts.get("stop_on_watch", true)
   def prune_unsat = opts.get("prune_unsat", true)
   while left > 0 {
      mut active = []
      mut dead = mgr.get("deadended", [])
      mut err = mgr.get("errored", [])
      mut unsat = mgr.get("unsat", [])
      def states = mgr.get("active", [])
      mut i = 0
      while i < states.len {
         def before = states[i]
         def before_snap = state_snapshot(before, opts.get("diff", dict()))
         def succ = _lifted_step_state(before, rows, opts)
         mut j = 0
         while j < succ.len {
            def after = succ[j]
            def after_snap = state_snapshot(after, opts.get("diff", dict()))
            def diff = state_diff(before_snap, after_snap, opts.get("diff", dict()))
            if state_diff_matches(diff, watch_pred) {
               watches = watches.append({"before": before_snap, "after": after_snap, "state": after, "diff": diff, "watch": watch_pred, "step": int(mgr.get("steps", 0)) + 1, "engine": "lifted"})
            }
            def parts = _classify_watch_successor(after, active, dead, err, unsat, prune_unsat)
            active = parts[0]
            dead = parts[1]
            err = parts[2]
            unsat = parts[3]
            j += 1
         }
         i += 1
      }
      mgr = mgr.set("active", active).set("deadended", dead).set("errored", err).set("unsat", unsat)
      .set("watch", watches).set("watch_count", watches.len).set("steps", int(mgr.get("steps", 0)) + 1)
      if active.len == 0 || (stop_on_watch && watches.len > 0) { break }
      left -= 1
   }
   mgr
}

fn _match_addr(any target, any addr) bool {
   if is_int(target) { return int(target) == int(addr) }
   if is_list(target) { return target.contains(addr) }
   false
}

fn find(any target) dict {
   "Create a find predicate record for explore()."
   {"kind": "find", "target": target}
}

fn avoid(any target) dict {
   "Create an avoid predicate record for explore()."
   {"kind": "avoid", "target": target}
}

fn find_reg(str reg, any value) dict {
   "Create a find predicate that matches a concrete/evaluable register value."
   {"kind": "find", "pred": "reg_eq", "reg": reg, "value": value}
}

fn avoid_reg(str reg, any value) dict {
   "Create an avoid predicate that matches a concrete/evaluable register value."
   {"kind": "avoid", "pred": "reg_eq", "reg": reg, "value": value}
}

fn find_mem(any addr, any bytes) dict {
   "Create a find predicate that matches concrete bytes at `addr`."
   {"kind": "find", "pred": "mem_eq", "addr": int(addr), "bytes": bytes}
}

fn avoid_mem(any addr, any bytes) dict {
   "Create an avoid predicate that matches concrete bytes at `addr`."
   {"kind": "avoid", "pred": "mem_eq", "addr": int(addr), "bytes": bytes}
}

fn find_stdout(any text) dict {
   "Create a find predicate that matches stdout containing `text`."
   {"kind": "find", "pred": "stdout_contains", "text": to_str(text)}
}

fn avoid_stdout(any text) dict {
   "Create an avoid predicate that matches stdout containing `text`."
   {"kind": "avoid", "pred": "stdout_contains", "text": to_str(text)}
}

fn find_stderr(any text) dict {
   "Create a find predicate that matches stderr containing `text`."
   {"kind": "find", "pred": "stderr_contains", "text": to_str(text)}
}

fn avoid_stderr(any text) dict {
   "Create an avoid predicate that matches stderr containing `text`."
   {"kind": "avoid", "pred": "stderr_contains", "text": to_str(text)}
}

fn find_output(any text) dict {
   "Create a find predicate that matches stdout or stderr containing `text`."
   {"kind": "find", "pred": "output_contains", "text": to_str(text)}
}

fn avoid_output(any text) dict {
   "Create an avoid predicate that matches stdout or stderr containing `text`."
   {"kind": "avoid", "pred": "output_contains", "text": to_str(text)}
}

fn _bytes_match(any actual, any expected) bool {
   if is_str(expected) || is_bytes(expected) {
      if is_str(actual) || is_bytes(actual) { return actual == expected }
      return false
   }
   if is_list(expected) {
      if is_list(actual) {
         if actual.len < expected.len { return false }
         mut i = 0
         while i < expected.len {
            if int(actual[i]) != int(expected[i]) { return false }
            i += 1
         }
         return true
      }
      if is_str(actual) || is_bytes(actual) {
         if actual.len < expected.len { return false }
         mut i = 0
         while i < expected.len {
            if (load8(actual, i) & 255) != (int(expected[i]) & 255) { return false }
            i += 1
         }
         return true
      }
   }
   actual == expected
}

fn _eval_reg_for_match(dict st, str reg) any {
   def v = state_get_reg(st, reg, nil)
   if v == nil { return nil }
   if is_int(v) { return v }
   state_eval_reg_u64(st, reg)
}

fn state_matches(dict st, any pred) bool {
   "Return true if `st` matches an address/register/memory/output predicate."
   if pred == 0 { return false }
   if !is_dict(pred) { return _match_addr(pred, state_addr(st)) }
   def p = pred.get("pred", "addr")
   if p == "addr" { return _match_addr(pred.get("target", 0), state_addr(st)) }
   if p == "reg_eq" {
      def got = _eval_reg_for_match(st, pred.get("reg", ""))
      if got == nil { return false }
      return got == pred.get("value", nil)
   }
   if p == "mem_eq" {
      def want = pred.get("bytes", "")
      def n = _data_len(want)
      if n <= 0 { return false }
      return _bytes_match(mem_read_bytes(st, int(pred.get("addr", 0)), n), want)
   }
   if p == "stdout_contains" { return contains(state_stdout(st), pred.get("text", "")) }
   if p == "stderr_contains" { return contains(state_stderr(st), pred.get("text", "")) }
   if p == "output_contains" { return contains(state_output(st), pred.get("text", "")) }
   false
}

fn explore(dict manager, any find_pred=0, any avoid_pred=0, int steps=0) dict {
   "Step and classify active states against find/avoid address targets."
   run_until(manager, find_pred, avoid_pred, steps)
}

fn _explore_find_limit(any opts) int {
   if !is_dict(opts) { return 1 }
   int(opts.get("find_limit", opts.get("num_find", opts.get("limit_found", 1))))
}

fn _explore_should_stop(dict mgr, int left, int find_limit) bool {
   if left <= 0 || mgr.get("active", []).len == 0 { return true }
   def found = mgr.get("found", []).len
   if find_limit <= 0 { return false }
   found >= find_limit
}

fn run_until(dict manager, any find_pred=0, any avoid_pred=0, int steps=0, any opts=dict()) dict {
   "Angr-like exploration loop. With `steps == 0`, only classify current
   states. With steps > 0, repeatedly steps active states and moves paths into
   found/avoid/deadended/errored buckets. `find_limit`/`num_find` keeps
   exploration alive until that many found states are collected ; pass <= 0 to
   keep going until steps or active states are exhausted."
   mut mgr = manager
   mut left = max(0, steps)
   mut iter = 0
   def find_limit = _explore_find_limit(opts)
   while true {
      mut active = []
      mut found = mgr.get("found", [])
      mut avoided = mgr.get("avoid", [])
      def ftarget = is_dict(find_pred) ? find_pred.get("target", 0) : find_pred
      def atarget = is_dict(avoid_pred) ? avoid_pred.get("target", 0) : avoid_pred
      def states = mgr.get("active", [])
      mut i = 0
      while i < states.len {
         def st = states[i]
         if state_matches(st, is_dict(find_pred) ? find_pred : ftarget) { found = found.append(st) }
         elif state_matches(st, is_dict(avoid_pred) ? avoid_pred : atarget) { avoided = avoided.append(st) }
         else { active = active.append(st) }
         i += 1
      }
      mgr = mgr.set("active", active).set("found", found).set("avoid", avoided)
      if _explore_should_stop(mgr, left, find_limit) { break }
      mgr = simgr_step(mgr, 1, opts)
      left -= 1
      iter += 1
   }
   mgr.set("steps", int(mgr.get("steps", 0)) + iter)
}

fn run_lifted_until(dict manager, list rows, any find_pred=0, any avoid_pred=0, int steps=0, any opts=dict()) dict {
   "Angr-like exploration loop driven by lifted decompiler rows.
   `find_limit`/`num_find` controls how many found paths to collect before
   stopping ; pass <= 0 to keep going until steps or active states are
   exhausted."
   mut mgr = manager
   mut left = max(0, steps)
   mut iter = 0
   def find_limit = _explore_find_limit(opts)
   while true {
      mut active = []
      mut found = mgr.get("found", [])
      mut avoided = mgr.get("avoid", [])
      def ftarget = is_dict(find_pred) ? find_pred.get("target", 0) : find_pred
      def atarget = is_dict(avoid_pred) ? avoid_pred.get("target", 0) : avoid_pred
      def states = mgr.get("active", [])
      mut i = 0
      while i < states.len {
         def st = states[i]
         if state_matches(st, is_dict(find_pred) ? find_pred : ftarget) { found = found.append(st) }
         elif state_matches(st, is_dict(avoid_pred) ? avoid_pred : atarget) { avoided = avoided.append(st) }
         else { active = active.append(st) }
         i += 1
      }
      mgr = mgr.set("active", active).set("found", found).set("avoid", avoided)
      if _explore_should_stop(mgr, left, find_limit) { break }
      mgr = simgr_lifted_step(mgr, rows, 1, opts)
      left -= 1
      iter += 1
   }
   mgr.set("steps", int(mgr.get("steps", 0)) + iter)
}
