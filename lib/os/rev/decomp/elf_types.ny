;; Keywords: elf machine symbols segments permissions
;; Stable ELF classification helpers shared by decompiler passes.
module std.os.rev.decomp.elf_types(machine, file_type, symbol_bind, symbol_type, segment_perms)
use std.core

fn machine(int id) str {
   case id {
      3 -> "x86"
      40 -> "arm"
      62 -> "x86_64"
      183 -> "aarch64"
      243 -> "riscv"
      _ -> "machine_" + to_str(id)
   }
}

fn file_type(int id) str {
   case id {
      1 -> "relocatable"
      2 -> "executable"
      3 -> "shared"
      4 -> "core"
      _ -> "unknown"
   }
}

fn symbol_bind(int info) str {
   case(info >> 4) {
      0 -> "local"
      1 -> "global"
      2 -> "weak"
      _ -> "bind_" + to_str(info >> 4)
   }
}

fn symbol_type(int info) str {
   case(info & 15) {
      0 -> "none"
      1 -> "object"
      2 -> "function"
      3 -> "section"
      4 -> "file"
      _ -> "type_" + to_str(info & 15)
   }
}

fn segment_perms(int flags) str {
   ((flags & 4) != 0 ? "r" : "-") + ((flags & 2) != 0 ? "w" : "-") +
   ((flags & 1) != 0 ? "x" : "-")
}

#main {
   assert(machine(62) == "x86_64" && file_type(2) == "executable", "ELF classifications")
   assert(symbol_bind(0x20) == "weak" && segment_perms(5) == "r-x", "ELF metadata")
   print("✓ std.os.rev.decomp.elf_types self-test passed")
}
