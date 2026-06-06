;; Keywords: assembly assembler disassembly disassembler capstone shellcode machine-code os
;; Assembly, disassembly, hexdump, and shellcode utilities backed by native disassembly tools.
;; References:
;; - std.os
module std.os.disasm(assemble, asm_hex, disasm, disasm_lines, hexdump, unhex,
   normalize_arch, arch_family, split_operands, instruction_kind, branch_condition,
   arithmetic_operator, clean_operand, operand_kind, memory_operand_parts,
   operand_ny_expr, address_ny_expr, is_zero_register, is_zeroing_idiom, target_address,
   shell_sh, shellcode_sh, shellcode_execve, shellcode_exit, shellcode_write, shellcode_read,
assembler_available, capstone_available)

use std.core
use std.core.str
use std.os (temp_dir, pid, file_write, file_read, file_remove)
use std.os.clock (ticks)
use std.os.subprocess as sub

#link "libcapstone.so"
#include <capstone/capstone.h> as "cs_"
extern "capstone" {
   fn _cs_version(ptr major, ptr minor) i32 as "cs_version"
   fn _cs_open(i32 arch, i32 mode, ptr h) i32 as "cs_open"
   fn _cs_close(ptr h) i32 as "cs_close"
   fn _cs_malloc(handle h) ptr as "cs_malloc"
   fn _cs_free(ptr insn, u64 count) as "cs_free"
   fn _cs_disasm_iter(handle h, ptr codep, ptr sizep, ptr addrp, ptr insn) i32 as "cs_disasm_iter"
}

def _CS_ARCH_ARM = 0
def _CS_ARCH_ARM64 = 1
def _CS_ARCH_X86 = 3
def _CS_ARCH_RISCV = 15
def _CS_MODE_ARM = 0
def _CS_MODE_16 = 2
def _CS_MODE_32 = 4
def _CS_MODE_64 = 8
def _CS_MODE_RISCV32 = 1
def _CS_MODE_RISCV64 = 2
def _CS_MODE_RISCVC = 4

fn assembler_available() bool {
   "Returns whether the external assembler toolchain is available."
   def r = sub.run_capture(["as", "--version"], [], nil, false)
   r.get("code", 1) == 0
}

fn capstone_available() bool {
   "Returns whether Capstone can be called through the native C API."
   def major = malloc(8)
   def minor = malloc(8)
   if(major == 0 || minor == 0){
      if(major != 0){ free(major) }
      if(minor != 0){ free(minor) }
      return false
   }
   defer { free(major) }
   defer { free(minor) }
   _cs_version(major, minor) > 0
}

fn _tmp_base() str {
   temp_dir() + "/nyasm_" + to_str(pid()) + "_" + to_str(ticks())
}

fn _asm_flags(str arch) list {
   def a = lower(strip(arch))
   if(a == "x86" || a == "i386" || a == "x86_32"){ return ["--32"] }
   if(a == "x86_64" || a == "amd64" || a == ""){ return ["--64"] }
   []
}

fn _asm_prelude(str arch, str syntax) str {
   def a = lower(strip(arch))
   def s = lower(strip(syntax))
   mut out = ""
   if(a == "x86_64" || a == "amd64" || a == "x86" || a == "i386" || a == "x86_32" || a == ""){
      if(s != "att"){ out = out + ".intel_syntax noprefix\n" }
   }
   out + ".global _start\n_start:\n"
}

fn assemble(str source, str arch="x86_64", str syntax="intel") str {
   "Assembles `source` into raw `.text` bytes using GNU as and objcopy."
   def base = _tmp_base()
   def src = base + ".s"
   def obj = base + ".o"
   def bin = base + ".bin"
   defer {
      match file_remove(src){ ok(a) -> { a } err(e) -> { e } }
      match file_remove(obj){ ok(a) -> { a } err(e) -> { e } }
      match file_remove(bin){ ok(a) -> { a } err(e) -> { e } }
   }
   match file_write(src, _asm_prelude(arch, syntax) + source + "\n"){
      ok(w) -> { w }
      err(e) -> { panic("assembler source write failed: " + repr(e)) }
   }
   def as_args = _asm_flags(arch).append("-o").append(obj).append(src)
   def ar = sub.run_capture("as", as_args, nil, false)
   if(ar.get("code", 1) != 0){ panic("assembler failed: " + ar.get("stdout", "")) }
   def or = sub.run_capture("objcopy", ["-O", "binary", "-j", ".text", obj, bin], nil, false)
   if(or.get("code", 1) != 0){ panic("objcopy failed: " + or.get("stdout", "")) }
   def rr = file_read(bin)
   if(is_err(rr)){ panic("assembler output read failed: " + repr(rr)) }
   unwrap(rr)
}

fn hexdump(any data, str sep="") str {
   "Returns lowercase hex for a byte string or byte list."
   mut out = ""
   if(is_str(data) || is_bytes(data)){
      mut i = 0
      while(i < data.len){
         if(i > 0){ out = out + sep }
         out = out + to_hex(load8(data, i), 2)
         i += 1
      }
      return out
   }
   if(is_list(data)){
      mut i = 0
      while(i < data.len){
         if(i > 0){ out = out + sep }
         out = out + to_hex(int(data[i]) & 255, 2)
         i += 1
      }
   }
   out
}

fn asm_hex(str source, str arch="x86_64", str syntax="intel") str {
   "Assembles source and returns raw bytes as hex."
   hexdump(assemble(source, arch, syntax))
}

fn _hex_val(int c) int {
   if(c >= 48 && c <= 57){ return c - 48 }
   if(c >= 65 && c <= 70){ return c - 55 }
   if(c >= 97 && c <= 102){ return c - 87 }
   -1
}

fn unhex(str text) str {
   "Converts a hex string into raw bytes. Whitespace, `0x`, `_`, `:`, and `-` are ignored."
   mut clean = ""
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      if(c == 48 && i + 1 < text.len && (load8(text, i + 1) == 120 || load8(text, i + 1) == 88)){
         i += 2
      } elif(_hex_val(c) >= 0){
         clean = clean + chr(c)
         i += 1
      } else {
         i += 1
      }
   }
   mut out = ""
   mut j = 0
   while(j + 1 < clean.len){
      def hi = _hex_val(load8(clean, j))
      def lo = _hex_val(load8(clean, j + 1))
      out = out + chr((hi << 4) | lo)
      j += 2
   }
   out
}

fn _bytestr(any data) str {
   if(is_str(data) || is_bytes(data)){ return data }
   if(is_list(data)){
      def base = malloc(max(1, data.len) + 16)
      if(base == 0){ return "" }
      def p = base + 16
      mut i = 0
      while(i < data.len){
         store8(p, int(data[i]) & 255, i)
         i += 1
      }
      return init_str(p, data.len)
   }
   ""
}

fn _cs_arch_mode(str arch) list {
   def a = normalize_arch(arch)
   if(a == "x86" || a == "i386" || a == "x86_32"){ return [_CS_ARCH_X86, _CS_MODE_32] }
   if(a == "x86_16" || a == "i8086"){ return [_CS_ARCH_X86, _CS_MODE_16] }
   if(a == "arm64" || a == "aarch64"){ return [_CS_ARCH_ARM64, _CS_MODE_ARM] }
   if(a == "arm"){ return [_CS_ARCH_ARM, _CS_MODE_ARM] }
   if(a == "riscv32" || a == "rv32"){ return [_CS_ARCH_RISCV, _CS_MODE_RISCV32] }
   if(a == "riscv64c" || a == "rv64c"){ return [_CS_ARCH_RISCV, _CS_MODE_RISCV64 | _CS_MODE_RISCVC] }
   if(a == "riscv32c" || a == "rv32c"){ return [_CS_ARCH_RISCV, _CS_MODE_RISCV32 | _CS_MODE_RISCVC] }
   if(a == "riscv" || a == "riscv64" || a == "rv64"){ return [_CS_ARCH_RISCV, _CS_MODE_RISCV64] }
   [_CS_ARCH_X86, _CS_MODE_64]
}

fn normalize_arch(str arch) str {
   "Returns a canonical architecture name understood by the disassembler."
   def a = lower(strip(arch))
   case a {
      "", "amd64", "x64" -> "x86_64"
      "i386", "i686", "x86_32" -> "x86"
      "arm64" -> "aarch64"
      "rv32" -> "riscv32"
      "rv64" -> "riscv64"
      "rv32c" -> "riscv32c"
      "rv64c" -> "riscv64c"
      _ -> a
   }
}

fn arch_family(str arch) str {
   "Returns the instruction-family bucket used by analysis code."
   def a = normalize_arch(arch)
   if(a == "x86" || a == "x86_64" || a == "x86_16" || a == "i8086"){ return "x86" }
   if(a == "aarch64"){ return "aarch64" }
   if(a == "arm"){ return "arm" }
   if(a == "riscv" || a == "riscv32" || a == "riscv64" || a == "riscv32c" || a == "riscv64c"){ return "riscv" }
   "unknown"
}

fn _is_riscv_branch(str m) bool {
   m == "beq" || m == "bne" || m == "blt" || m == "bge" || m == "bltu" || m == "bgeu" || m == "bgt" || m == "ble" || m == "bgtu" || m == "bleu"
}

fn split_operands(str ops) list {
   "Splits an operand string on top-level commas while preserving memory expressions."
   mut out = []
   mut start = 0
   mut depth = 0
   mut i = 0
   while(i < ops.len){
      def c = load8(ops, i)
      if(c == 91 || c == 40 || c == 123){ depth += 1 }
      elif(c == 93 || c == 41 || c == 125){ depth = max(0, depth - 1) }
      elif(c == 44 && depth == 0){
         out = out.append(strip(slice(ops, start, i, 1)))
         start = i + 1
      }
      i += 1
   }
   if(start < ops.len){ out = out.append(strip(slice(ops, start, ops.len, 1))) }
   out
}

fn _drop_prefix(str s, str p) str {
   startswith(s, p) ? strip(slice(s, p.len, s.len, 1)) : s
}

fn clean_operand(str op) str {
   "Removes common assembler decoration from one operand while preserving addressing text."
   mut s = strip(op)
   s = _drop_prefix(s, "byte ptr ")
   s = _drop_prefix(s, "word ptr ")
   s = _drop_prefix(s, "dword ptr ")
   s = _drop_prefix(s, "qword ptr ")
   s = _drop_prefix(s, "xmmword ptr ")
   s = _drop_prefix(s, "ymmword ptr ")
   if(startswith(s, "#")){ s = strip(slice(s, 1, s.len, 1)) }
   s
}

fn _mem_term(str raw) str {
   mut s = strip(raw)
   if(startswith(s, "#")){ s = strip(slice(s, 1, s.len, 1)) }
   s
}

fn _mem_expr_from_terms(list terms) str {
   if(terms.len == 0){ return "" }
   mut expr = _mem_term(terms[0])
   mut i = 1
   while(i < terms.len){
      def t = _mem_term(terms[i])
      if(startswith(t, "-")){ expr = expr + " - " + strip(slice(t, 1, t.len, 1)) }
      elif(startswith(t, "+")){ expr = expr + " + " + strip(slice(t, 1, t.len, 1)) }
      else { expr = expr + " + " + t }
      i += 1
   }
   expr
}

fn _plain_mem_base(str raw) bool {
   def s = _mem_term(raw)
   if(s.len == 0){ return false }
   if(startswith(s, "0x") || startswith(s, "-0x") || str.ascii_is_digit(load8(s, 0))){ return false }
   str.find(s, "+") < 0 && str.find(s, "*") < 0 && str.find(s, "-") <= 0
}

fn memory_operand_parts(str op) dict {
   "Returns normalized memory-address pieces for `[base + off]` and `off(base)` operands."
   mut s = clean_operand(op)
   if(str.endswith(s, "!")){ s = strip(slice(s, 0, s.len - 1, 1)) }
   if(startswith(s, "fs:[") || startswith(s, "gs:[")){
      def seg = slice(s, 0, 2, 1)
      def inner = slice(s, 4, s.len - 1, 1)
      def terms = split_operands(inner)
      mut out = {"ok": true, "segment": seg, "expr": _mem_expr_from_terms(terms)}
      if(terms.len > 1 || (terms.len == 1 && _plain_mem_base(terms[0]))){ out = out.set("base", _mem_term(terms[0])) }
      if(terms.len > 1){ out = out.set("offset", _mem_term(terms[1])) }
      return out
   }
   def lb = str.find(s, "[")
   if(lb >= 0 && str.endswith(s, "]")){
      def inner = slice(s, lb + 1, s.len - 1, 1)
      def terms = split_operands(inner)
      mut out = {"ok": true, "segment": "", "expr": _mem_expr_from_terms(terms)}
      if(terms.len > 1 || (terms.len == 1 && _plain_mem_base(terms[0]))){ out = out.set("base", _mem_term(terms[0])) }
      if(terms.len > 1){ out = out.set("offset", _mem_term(terms[1])) }
      return out
   }
   def open = str.find(s, "(")
   if(open >= 0 && str.endswith(s, ")")){
      def off = strip(slice(s, 0, open, 1))
      def base = strip(slice(s, open + 1, s.len - 1, 1))
      if(base.len == 0){ return dict() }
      if(off.len == 0 || off == "0"){ return {"ok": true, "segment": "", "expr": base, "base": base, "offset": "0"} }
      if(startswith(off, "-")){ return {"ok": true, "segment": "", "expr": base + " - " + slice(off, 1, off.len, 1), "base": base, "offset": off} }
      return {"ok": true, "segment": "", "expr": base + " + " + off, "base": base, "offset": off}
   }
   dict()
}

fn operand_kind(str op) str {
   "Classifies one operand as `none`, `mem`, `imm`, `expr`, or `reg`."
   def s = clean_operand(op)
   if(s.len == 0){ return "none" }
   if(memory_operand_parts(s).get("ok", false)){ return "mem" }
   if(startswith(s, "#") || startswith(s, "0x") || startswith(s, "-0x") || str.ascii_is_digit(load8(s, 0)) || load8(s, 0) == 45){ return "imm" }
   if(str.find(s, "+") >= 0 || str.find(s, "-") > 0 || str.find(s, "*") >= 0){ return "expr" }
   "reg"
}

fn operand_ny_expr(str op) str {
   "Returns a compact Ny-style expression for one decoded operand."
   def s = clean_operand(op)
   def mem = memory_operand_parts(s)
   if(mem.get("ok", false)){
      def seg = mem.get("segment", "")
      def expr = mem.get("expr", "")
      if(seg == "fs"){ return "mem_fs(" + expr + ")" }
      if(seg == "gs"){ return "mem_gs(" + expr + ")" }
      return "mem(" + expr + ")"
   }
   str.str_replace(s, " ", "_")
}

fn address_ny_expr(str op) str {
   "Returns a Ny-style address expression for operands used as addresses, such as x86 `lea` sources."
   def n = operand_ny_expr(op)
   if(startswith(n, "mem(")){ return slice(n, 4, n.len - 1, 1) }
   if(startswith(n, "mem_fs(")){ return "&" + slice(n, 6, n.len, 1) }
   if(startswith(n, "mem_gs(")){ return "&" + slice(n, 6, n.len, 1) }
   "&" + n
}

fn instruction_kind(str arch, str mnemonic) str {
   "Classifies a decoded instruction independent of the concrete ISA syntax."
   def fam = arch_family(arch)
   def m = lower(strip(mnemonic))
   if(m == "syscall" || m == "svc" || m == "ecall"){ return "syscall" }
   if(fam == "x86"){
      if(startswith(m, "call")){ return "call" }
      if(startswith(m, "ret")){ return "return" }
      if(startswith(m, "j")){ return "branch" }
      if(m == "push" || m == "pop" || m == "leave"){ return "stack" }
      if(startswith(m, "mov") || startswith(m, "cmov") || startswith(m, "set") || m == "lea" || startswith(m, "xchg")){ return "assign" }
      if(startswith(m, "cmp") || startswith(m, "test")){ return "compare" }
      if(startswith(m, "add") || startswith(m, "sub") || startswith(m, "imul") || m == "mul" || startswith(m, "xor") || startswith(m, "and") || startswith(m, "or") || startswith(m, "inc") || startswith(m, "dec") || startswith(m, "neg") || m == "not" || startswith(m, "shl") || startswith(m, "shr") || startswith(m, "sar") || startswith(m, "rol") || startswith(m, "ror") ||
      m == "cdq" || m == "cqo" || m == "idiv" || m == "div"){ return "arith" }
      return "insn"
   }
   if(fam == "aarch64" || fam == "arm"){
      if(m == "bl" || m == "blr"){ return "call" }
      if(m == "ret" || m == "bx" || m == "pop"){ return "return" }
      if(startswith(m, "b.") || m == "b" || m == "br" || m == "beq" || m == "bne" || m == "bgt" || m == "bge" || m == "blt" || m == "ble" || m == "bhi" || m == "bhs" || m == "blo" || m == "bls" || m == "bmi" || m == "bpl" || m == "cbz" || m == "cbnz" || m == "tbz" || m == "tbnz"){ return "branch" }
      if(m == "cmp" || m == "cmn" || m == "tst"){ return "compare" }
      if(startswith(m, "mov") || m == "adr" || m == "adrp" || startswith(m, "ldr") || startswith(m, "ldur") ||
      startswith(m, "str") || startswith(m, "stur") || m == "ldp" || m == "stp" || m == "ldnp" || m == "stnp"){ return "assign" }
      if(startswith(m, "add") || startswith(m, "sub") || startswith(m, "eor") || startswith(m, "orr") || startswith(m, "and") || startswith(m, "lsl") || startswith(m, "lsr") || startswith(m, "asr") ||
      startswith(m, "mul") || m == "sdiv" || m == "udiv"){ return "arith" }
      return "insn"
   }
   if(fam == "riscv"){
      if(m == "jal" || m == "jalr" || m == "call"){ return "call" }
      if(m == "ret"){ return "return" }
      if(_is_riscv_branch(m) || m == "j"){ return "branch" }
      if(startswith(m, "li") || startswith(m, "mv") || startswith(m, "ld") || startswith(m, "lw") || startswith(m, "lh") || startswith(m, "lb") || startswith(m, "sd") || startswith(m, "sw") || startswith(m, "sh") || startswith(m, "sb")){ return "assign" }
      if(startswith(m, "add") || startswith(m, "sub") || startswith(m, "xor") || startswith(m, "or") || startswith(m, "and") || startswith(m, "sll") || startswith(m, "srl") || startswith(m, "sra") ||
      startswith(m, "mul") || startswith(m, "div") || startswith(m, "rem") || startswith(m, "slt")){ return "arith" }
   }
   "insn"
}

fn arithmetic_operator(str mnemonic) str {
   "Returns a compact operator token for common arithmetic/logical mnemonics."
   def m = lower(strip(mnemonic))
   if(startswith(m, "add")){ return "+" }
   if(startswith(m, "sub")){ return "-" }
   if(startswith(m, "imul") || startswith(m, "mul")){ return "*" }
   if(startswith(m, "xor") || startswith(m, "eor")){ return "^^" }
   if(startswith(m, "and")){ return "&" }
   if(startswith(m, "or") || startswith(m, "orr")){ return "|" }
   if(startswith(m, "neg")){ return "neg" }
   if(m == "not"){ return "~" }
   if(startswith(m, "shl") || startswith(m, "sll") || startswith(m, "lsl")){ return "<<" }
   if(startswith(m, "shr") || startswith(m, "sar") || startswith(m, "srl") || startswith(m, "sra") || startswith(m, "lsr") || startswith(m, "asr")){ return ">>" }
   if(startswith(m, "rol")){ return "rol" }
   if(startswith(m, "ror")){ return "ror" }
   if(m == "idiv" || m == "div" || startswith(m, "div") || m == "sdiv" || m == "udiv"){ return "/" }
   if(startswith(m, "rem")){ return "%" }
   if(startswith(m, "slt")){ return "<" }
   m
}

fn is_zero_register(str arch, str reg) bool {
   "Returns true for ISA registers that always read as zero."
   def fam = arch_family(arch)
   def r = lower(strip(reg))
   if(fam == "riscv"){ return r == "zero" || r == "x0" }
   false
}

fn _same_operand(str a, str b) bool {
   lower(strip(a)) == lower(strip(b))
}

fn is_zeroing_idiom(str arch, str mnemonic, str operands) bool {
   "Recognizes common register-zeroing idioms from decoded text."
   def fam = arch_family(arch)
   def m = lower(strip(mnemonic))
   def parts = split_operands(operands)
   if(parts.len < 2){ return false }
   if(fam == "x86"){
      return(startswith(m, "xor") || startswith(m, "sub")) && _same_operand(parts[0], parts[1])
   }
   if(fam == "aarch64" || fam == "arm"){
      return(startswith(m, "eor") || startswith(m, "sub")) && parts.len > 2 && _same_operand(parts[1], parts[2])
   }
   if(fam == "riscv"){
      return(startswith(m, "xor") || startswith(m, "sub")) && parts.len > 2 && _same_operand(parts[1], parts[2])
   }
   false
}

fn branch_condition(str arch, str mnemonic) str {
   "Returns a normalized condition name for a branch mnemonic."
   def fam = arch_family(arch)
   def m = lower(strip(mnemonic))
   if(fam == "x86"){
      case m {
         "je" -> "eq"
         "jz" -> "eq"
         "jne" -> "ne"
         "jnz" -> "ne"
         "ja" -> "ugt"
         "jae" -> "uge"
         "jb" -> "ult"
         "jbe" -> "ule"
         "jg" -> "gt"
         "jge" -> "ge"
         "jl" -> "lt"
         "jle" -> "le"
         "js" -> "negative"
         "jns" -> "non_negative"
         _ -> startswith(m, "jmp") ? "always" : m
      }
   } elif(fam == "aarch64" || fam == "arm"){
      if(m == "b" || m == "br"){ return "always" }
      if(m == "cbz"){ return "zero" }
      if(m == "cbnz"){ return "nonzero" }
      if(m == "tbz"){ return "bit_zero" }
      if(m == "tbnz"){ return "bit_nonzero" }
      if(startswith(m, "b.")){
         case slice(m, 2, m.len, 1){
            "eq" -> "eq"
            "ne" -> "ne"
            "gt" -> "gt"
            "ge" -> "ge"
            "lt" -> "lt"
            "le" -> "le"
            "hi" -> "ugt"
            "hs" -> "uge"
            "cs" -> "uge"
            "lo" -> "ult"
            "cc" -> "ult"
            "ls" -> "ule"
            "mi" -> "negative"
            "pl" -> "non_negative"
            _ -> slice(m, 2, m.len, 1)
         }
      }
      case m {
         "beq" -> "eq"
         "bne" -> "ne"
         "bgt" -> "gt"
         "bge" -> "ge"
         "blt" -> "lt"
         "ble" -> "le"
         _ -> m
      }
   } elif(fam == "riscv"){
      case m {
         "j" -> "always"
         "beq" -> "eq"
         "bne" -> "ne"
         "blt" -> "lt"
         "bge" -> "ge"
         "bltu" -> "ult"
         "bgeu" -> "uge"
         "bgt" -> "gt"
         "ble" -> "le"
         "bgtu" -> "ugt"
         "bleu" -> "ule"
         _ -> m
      }
   } else {
      m
   }
}

fn _target_text(any row_or_operands) str {
   if(is_list(row_or_operands) && row_or_operands.len > 2){ return to_str(row_or_operands[2]) }
   to_str(row_or_operands)
}

fn target_address(any row_or_operands, str arch="") int {
   "Returns a direct code target in an operand string or disassembly row."
   def ops = _target_text(row_or_operands)
   def row_base = is_list(row_or_operands) && row_or_operands.len > 0 ? int(row_or_operands[0]) : 0
   def fam = arch_family(arch)
   mut i = 0
   while(i + 2 < ops.len){
      if(load8(ops, i) == 48 && (load8(ops, i + 1) == 120 || load8(ops, i + 1) == 88)){
         mut j = i + 2
         mut value = 0
         mut seen = false
         while(j < ops.len){
            def hv = _hex_val(load8(ops, j))
            if(hv < 0){ break }
            value = (value << 4) | hv
            seen = true
            j += 1
         }
         if(seen){ return value }
      }
      i += 1
   }
   mut start = ops.len
   while(start > 0 && load8(ops, start - 1) != 44){ start -= 1 }
   while(start < ops.len && (load8(ops, start) == 32 || load8(ops, start) == 9)){ start += 1 }
   mut sign = 1
   if(start < ops.len && load8(ops, start) == 45){ sign = -1 start += 1 }
   mut j = start
   mut value = 0
   mut seen = false
   while(j < ops.len){
      def c = load8(ops, j)
      if(c < 48 || c > 57){ break }
      value = value * 10 + (c - 48)
      seen = true
      j += 1
   }
   if(seen){
      def raw = sign * value
      if(row_base != 0 && fam == "riscv"){ return row_base + raw }
      return raw
   }
   0
}

fn _cstr_at(any p, int off, int maxn) str {
   mut b = Builder(maxn + 1)
   mut i = 0
   while(i < maxn){
      def c = load8(p, off + i)
      if(c == 0){ break }
      b = builder_append(b, chr(c))
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _copy_to_ptr(str data) ptr {
   def p = malloc(data.len + 1)
   if(p == 0){ return 0 }
   mut i = 0
   while(i < data.len){
      store8(p, load8(data, i), i)
      i += 1
   }
   store8(p, 0, data.len)
   p
}

fn disasm_lines(any code, str arch="x86_64", int address=0) list {
   "Disassembles raw bytes with Capstone and returns `[address, mnemonic, operands, size]` rows."
   def data = _bytestr(code)
   if(data.len == 0){ return [] }
   def am = _cs_arch_mode(arch)
   def hptr = malloc(8)
   if(hptr == 0){ return [] }
   defer { free(hptr) }
   if(_cs_open(am[0], am[1], hptr) != 0){ return [] }
   defer { _cs_close(hptr) }
   def handle = load64_h(hptr, 0)
   def insn = _cs_malloc(handle)
   if(insn == 0){ return [] }
   defer { _cs_free(insn, 1) }
   def raw = _copy_to_ptr(data)
   if(raw == 0){ return [] }
   defer { free(raw) }
   def codep = malloc(8)
   def sizep = malloc(8)
   def addrp = malloc(8)
   if(codep == 0 || sizep == 0 || addrp == 0){ return [] }
   defer {
      free(codep)
      free(sizep)
      free(addrp)
   }
   store64_h(codep, raw, 0)
   store64_h(sizep, data.len, 0)
   store64_h(addrp, address, 0)
   mut rows = []
   while(load64_h(sizep, 0) > 0){
      def ok = _cs_disasm_iter(handle, codep, sizep, addrp, insn)
      if(ok == 0){ break }
      rows = rows.append([load64_h(insn, 8), _cstr_at(insn, 42, 32), _cstr_at(insn, 74, 160), load16(insn, 16)])
   }
   rows
}

fn disasm(any code, str arch="x86_64", int address=0) str {
   "Disassembles raw bytes with Capstone and returns printable text."
   def rows = disasm_lines(code, arch, address)
   mut b = Builder(128)
   mut i = 0
   while(i < rows.len){
      def row = rows[i]
      def ops = row[2]
      b = builder_append(b, "0x" + to_hex(row[0], 0) + ": " + row[1])
      if(is_str(ops) && ops.len > 0){ b = builder_append(b, " " + ops) }
      b = builder_append(b, "\n")
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _asm_u64_hex(int v) str {
   "0x" + to_hex(v, 0)
}

fn _stack_string(str s) str {
   mut data = s
   if(data.len == 0 || load8(data, data.len - 1) != 0){ data = data + chr(0) }
   mut out = ""
   mut stop = data.len
   while(stop > 0){
      def start = max(0, stop - 8)
      mut val = 0
      mut shift = 0
      mut i = start
      while(i < stop){
         val = val | (load8(data, i) << shift)
         shift += 8
         i += 1
      }
      out = out + "mov rbx, " + _asm_u64_hex(val) + "\npush rbx\n"
      stop = start
   }
   out
}

fn shell_sh() str {
   "Returns a tiny Linux/x86_64 `/bin/sh` execve assembly snippet."
   shellcode_execve("/bin/sh")
}

fn shellcode_sh() str {
   "Alias for `shell_sh`."
   shell_sh()
}

fn shellcode_execve(str path="/bin/sh") str {
   "Returns Linux/x86_64 assembly for `execve(path, [path], NULL)`."
   "xor rdx, rdx\n" +
   _stack_string(path) +
   "mov rdi, rsp\n" +
   "push rdx\n" +
   "push rdi\n" +
   "mov rsi, rsp\n" +
   "mov eax, 59\n" +
   "syscall"
}

fn shellcode_exit(int code=0) str {
   "Returns Linux/x86_64 assembly for `exit(code)`."
   "mov edi, " + to_str(code) + "\nmov eax, 60\nsyscall"
}

fn shellcode_write(int fd, str text) str {
   "Returns Linux/x86_64 assembly for `write(fd, text, len(text))`."
   _stack_string(text) +
   "mov rsi, rsp\n" +
   "mov edx, " + to_str(text.len) + "\n" +
   "mov edi, " + to_str(fd) + "\n" +
   "mov eax, 1\n" +
   "syscall"
}

fn shellcode_read(int fd=0, int count=4096) str {
   "Returns Linux/x86_64 assembly for `read(fd, rsp-count, count)`. The buffer pointer remains in `rsi`."
   "sub rsp, " + to_str(max(1, count)) + "\n" +
   "mov rsi, rsp\n" +
   "mov edx, " + to_str(max(1, count)) + "\n" +
   "mov edi, " + to_str(fd) + "\n" +
   "xor eax, eax\n" +
   "syscall"
}

#main {
   assert(clean_operand("qword ptr [rip + 0x20]") == "[rip + 0x20]", "disasm clean operand")
   assert(operand_kind("[rip + 0x20]") == "mem" && operand_kind("0x401000") == "imm" && operand_kind("rax + 4") == "expr", "disasm operand kind")
   assert(operand_ny_expr("[rsp + 0x20]") == "mem(rsp + 0x20)" && address_ny_expr("[rsp + 0x20]") == "rsp + 0x20", "disasm x86 memory expr")
   assert(operand_ny_expr("fs:[0x28]") == "mem_fs(0x28)" && operand_ny_expr("gs:[rax + 8]") == "mem_gs(rax + 8)", "disasm segment memory expr")
   assert(operand_ny_expr("8(sp)") == "mem(sp + 8)" && address_ny_expr("8(sp)") == "sp + 8", "disasm riscv memory expr")
   def rv = memory_operand_parts("8(sp)")
   assert(rv.get("ok", false) && rv.get("base", "") == "sp" && rv.get("offset", "") == "8", "disasm riscv memory parts")
   assert(operand_kind("[sp, #-0x10]!") == "mem" && operand_ny_expr("[sp, #-0x10]!") == "mem(sp - 0x10)", "disasm aarch64 writeback expr")
   def arm = memory_operand_parts("[sp, #-0x10]!")
   assert(arm.get("ok", false) && arm.get("base", "") == "sp" && arm.get("offset", "") == "-0x10", "disasm aarch64 memory parts")
   assert(split_operands("rax, [rip + 0x20], {x0, x1}").len == 3, "disasm split operands")
   assert(instruction_kind("x86_64", "cdq") == "arith" && instruction_kind("x86_64", "idiv") == "arith" && arithmetic_operator("idiv") == "/", "disasm x86 division")
   assert(instruction_kind("x86_64", "imul") == "arith" && arithmetic_operator("imul") == "*", "disasm x86 multiply")
   assert(instruction_kind("x86_64", "rol") == "arith" && arithmetic_operator("ror") == "ror", "disasm x86 rotate")
   assert(instruction_kind("x86_64", "not") == "arith" && arithmetic_operator("not") == "~", "disasm x86 not")
   assert(instruction_kind("x86_64", "neg") == "arith" && arithmetic_operator("neg") == "neg", "disasm x86 neg")
   assert(instruction_kind("x86_64", "xchg") == "assign" && instruction_kind("aarch64", "stp") == "assign" && instruction_kind("aarch64", "ldp") == "assign", "disasm assign kind")
   assert(arithmetic_operator("sdiv") == "/" && instruction_kind("riscv64", "rem") == "arith" && arithmetic_operator("rem") == "%", "disasm non-x86 arithmetic")
   print("✓ std.os.disasm self-test passed")
}
