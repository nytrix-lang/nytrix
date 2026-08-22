;; Keywords: abi registers calling-convention stack immediates
;; ABI, register-alias, stack-slot, and immediate parsing primitives.
module std.os.rev.decomp.abi *
use std.core
use std.core.str as str

fn _parse_int_piece(str raw) int {
   mut s = str.strip(raw)
   mut sign = 1
   if str.startswith(s, "-") {
      sign = -1
      s = str.strip(slice(s, 1, s.len, 1))
   }
   if str.startswith(s, "+") { s = str.strip(slice(s, 1, s.len, 1)) }
   if str.startswith(s, "0x") || str.startswith(s, "0X") { return sign * str.parse_int(slice(s, 2, s.len, 1), 16) }
   if s.len == 0 { return 0 }
   sign * str.parse_int(s, 10)
}

fn _imm_value(str op) int {
   mut s = str.strip(op)
   if str.startswith(s, "#") { s = str.strip(slice(s, 1, s.len, 1)) }
   if s.len == 0 { return 0 }
   if str.startswith(s, "0x") || str.startswith(s, "-0x") || str.ascii_is_digit(load8(s, 0)) || load8(s, 0) == 45 { return _parse_int_piece(s) }
   0
}

fn _iabs(int v) int {
   v < 0 ? -v : v
}

fn _disp_after(str ops, str base) int {
   def p = str.find(ops, base)
   if p < 0 { return 0 }
   mut i = p + base.len
   while i < ops.len && str.ascii_is_space(load8(ops, i)) { i += 1 }
   mut sign = 1
   if i < ops.len && load8(ops, i) == 45 { sign = -1 i += 1 }
   elif i < ops.len && load8(ops, i) == 43 { i += 1 }
   else { return 0 }
   while i < ops.len && str.ascii_is_space(load8(ops, i)) { i += 1 }
   mut end = i
   if end + 1 < ops.len && load8(ops, end) == 48 && (load8(ops, end + 1) == 120 || load8(ops, end + 1) == 88) {
      end += 2
      while end < ops.len && str.ascii_is_hex_digit(load8(ops, end)) { end += 1 }
      return sign * str.parse_int(slice(ops, i + 2, end, 1), 16)
   }
   while end < ops.len && str.ascii_is_digit(load8(ops, end)) { end += 1 }
   if end <= i { return 0 }
   sign * str.parse_int(slice(ops, i, end, 1), 10)
}

fn _stack_slot_for_ops(str ops) dict {
   mut base = ""
   mut off = 0
   if str.find(ops, "rbp") >= 0 {
      base = "rbp"
      off = _disp_after(ops, "rbp")
   } elif str.find(ops, "ebp") >= 0 {
      base = "ebp"
      off = _disp_after(ops, "ebp")
   } elif str.find(ops, "rsp") >= 0 {
      base = "rsp"
      off = _disp_after(ops, "rsp")
   } elif str.find(ops, "esp") >= 0 {
      base = "esp"
      off = _disp_after(ops, "esp")
   }
   if base.len == 0 || off == 0 { return dict() }
   {"base": base, "offset": off, "name": (off < 0 ? "local_" : "stack_") + str.to_hex(_iabs(off), 0)}
}

fn _stack_slot_renderable(dict slot) bool {
   if slot.len == 0 { return false }
   def base = slot.get("base", "")
   def off = int(slot.get("offset", 0))
   if off < 0 { return true }
   base == "rsp" || base == "esp"
}

fn _arg_aliases(str reg) list {
   case reg {
      "rax" -> ["rax", "eax", "ax", "al"]
      "rbx" -> ["rbx", "ebx", "bx", "bl"]
      "rbp" -> ["rbp", "ebp", "bp", "bpl"]
      "rsp" -> ["rsp", "esp", "sp", "spl"]
      "rdi" -> ["rdi", "edi", "di", "dil"]
      "rsi" -> ["rsi", "esi", "si", "sil"]
      "rdx" -> ["rdx", "edx", "dx", "dl"]
      "rcx" -> ["rcx", "ecx", "cx", "cl"]
      "r10" -> ["r10", "r10d", "r10w", "r10b"]
      "r11" -> ["r11", "r11d", "r11w", "r11b"]
      "r12" -> ["r12", "r12d", "r12w", "r12b"]
      "r13" -> ["r13", "r13d", "r13w", "r13b"]
      "r14" -> ["r14", "r14d", "r14w", "r14b"]
      "r15" -> ["r15", "r15d", "r15w", "r15b"]
      "r8" -> ["r8", "r8d", "r8w", "r8b"]
      "r9" -> ["r9", "r9d", "r9w", "r9b"]
      "x0" -> ["x0", "w0"]
      "x1" -> ["x1", "w1"]
      "x2" -> ["x2", "w2"]
      "x3" -> ["x3", "w3"]
      "x4" -> ["x4", "w4"]
      "x5" -> ["x5", "w5"]
      "x6" -> ["x6", "w6"]
      "x7" -> ["x7", "w7"]
      "x8" -> ["x8", "w8"]
      "r0" -> ["r0"]
      "r1" -> ["r1"]
      "r2" -> ["r2"]
      "r3" -> ["r3"]
      "r4" -> ["r4"]
      "r5" -> ["r5"]
      "r7" -> ["r7"]
      "a0" -> ["a0", "x10"]
      "a1" -> ["a1", "x11"]
      "a2" -> ["a2", "x12"]
      "a3" -> ["a3", "x13"]
      "a4" -> ["a4", "x14"]
      "a5" -> ["a5", "x15"]
      "a6" -> ["a6", "x16"]
      "a7" -> ["a7", "x17"]
      _ -> [reg]
   }
}

fn _abi_profile(str family) dict {
   if family == "aarch64" { return {"abi": "aapcs64", "args": ["x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"], "returns": ["x0"]} }
   if family == "arm" { return {"abi": "aapcs32", "args": ["r0", "r1", "r2", "r3"], "returns": ["r0"]} }
   if family == "riscv" { return {"abi": "riscv_elf", "args": ["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"], "returns": ["a0"]} }
   {"abi": "sysv_x86_64", "args": ["rdi", "rsi", "rdx", "rcx", "r8", "r9"], "returns": ["rax"]}
}

fn _rows_family(list rows) str {
   mut i = 0
   while i < rows.len {
      def f = rows[i].get("family", "")
      if f.len > 0 { return f }
      i += 1
   }
   "x86"
}

fn _alias_reg_for(list regs, str op) str {
   def s = str.strip(op)
   mut i = 0
   while i < regs.len {
      def al = _arg_aliases(regs[i])
      mut j = 0
      while j < al.len {
         if s == al[j] { return regs[i] }
         j += 1
      }
      i += 1
   }
   ""
}

fn _alias_reg(str op) str {
   _alias_reg_for(["rax", "rdi", "rsi", "rdx", "rcx", "r10", "r8", "r9", "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "r0", "r1", "r2", "r3", "r4", "r5", "r7", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"], op)
}

fn _cmp_condition_symbol(str cond) str {
   case cond {
      "eq" -> "=="
      "ne" -> "!="
      "gt" -> ">"
      "ge" -> ">="
      "lt" -> "<"
      "le" -> "<="
      "ugt" -> ">"
      "uge" -> ">="
      "ult" -> "<"
      "ule" -> "<="
      "negative" -> "< 0"
      "non_negative" -> ">= 0"
      _ -> ""
   }
}

fn _imm_number(str s) int {
   mut x = str.strip(s)
   if str.startswith(x, "#") { x = str.strip(slice(x, 1, x.len, 1)) }
   if str.startswith(x, "0x") { return str.parse_int(slice(x, 2, x.len, 1), 16) }
   if str.startswith(x, "-0x") { return -str.parse_int(slice(x, 3, x.len, 1), 16) }
   str.parse_int(x, 10)
}
