;; Keywords: symbols registers aliases tokens def-use slicing
;; Register aliases and token extraction for def-use and slicing passes.
module std.os.rev.decomp.symbols(_symbol_is_name_char, _symbol_token_ok, _symbols_from_text, _slice_symbol_aliases)

use std.core
use std.core.str as str
use std.os.rev.decomp.collections (_append_unique)

fn _symbol_is_name_char(int ch) bool { str.ascii_is_alnum(ch) || ch == 95 }

fn _symbol_token_ok(str tok) bool {
   if tok.len == 0 { return false }
   if tok == "mem" || tok == "mem_fs" || tok == "mem_gs" || tok == "ptr" { return false }
   if tok == "byte" || tok == "word" || tok == "dword" || tok == "qword" { return false }
   if str.startswith(tok, "0x") { return false }
   !str.ascii_is_digit(load8(tok, 0))
}

fn _symbols_from_text(str raw) list {
   def s = str.lower(raw)
   mut out = []
   mut i = 0
   while i < s.len {
      while i < s.len && !_symbol_is_name_char(load8(s, i)) { i += 1 }
      def start = i
      while i < s.len && _symbol_is_name_char(load8(s, i)) { i += 1 }
      if i > start {
         def tok = slice(s, start, i, 1)
         if _symbol_token_ok(tok) { out = _append_unique(out, tok) }
      }
   }
   out
}

fn _slice_symbol_aliases(str sym) list {
   def s = str.lower(sym)
   case s {
      "rax", "eax", "ax", "al" -> ["rax", "eax", "ax", "al"]
      "rbx", "ebx", "bx", "bl" -> ["rbx", "ebx", "bx", "bl"]
      "rcx", "ecx", "cx", "cl" -> ["rcx", "ecx", "cx", "cl"]
      "rdx", "edx", "dx", "dl" -> ["rdx", "edx", "dx", "dl"]
      "rsi", "esi", "si", "sil" -> ["rsi", "esi", "si", "sil"]
      "rdi", "edi", "di", "dil" -> ["rdi", "edi", "di", "dil"]
      "rbp", "ebp", "bp", "bpl" -> ["rbp", "ebp", "bp", "bpl"]
      "rsp", "esp", "sp", "spl" -> ["rsp", "esp", "sp", "spl"]
      "r8", "r8d", "r8w", "r8b" -> ["r8", "r8d", "r8w", "r8b"]
      "r9", "r9d", "r9w", "r9b" -> ["r9", "r9d", "r9w", "r9b"]
      "r10", "r10d", "r10w", "r10b" -> ["r10", "r10d", "r10w", "r10b"]
      "r11", "r11d", "r11w", "r11b" -> ["r11", "r11d", "r11w", "r11b"]
      "r12", "r12d", "r12w", "r12b" -> ["r12", "r12d", "r12w", "r12b"]
      "r13", "r13d", "r13w", "r13b" -> ["r13", "r13d", "r13w", "r13b"]
      "r14", "r14d", "r14w", "r14b" -> ["r14", "r14d", "r14w", "r14b"]
      "r15", "r15d", "r15w", "r15b" -> ["r15", "r15d", "r15w", "r15b"]
      "rip", "eip" -> ["rip", "eip"]
      _ -> [s]
   }
}

#main {
   assert(_symbols_from_text("mem(rax + item_1)").len == 2, "symbol extraction")
   assert(_slice_symbol_aliases("eax").len == 4, "register aliases")
   print("✓ std.os.rev.decomp.symbols self-test passed")
}
