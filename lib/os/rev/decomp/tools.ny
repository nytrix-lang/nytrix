;; Keywords: tools demangle cxxfilt subprocess disassembly
;; External tool boundary for the Nytrix decompiler.
module std.os.rev.decomp.tools(tool_status, demangle)

use std.core
use std.core.str as str
use std.os.disasm as dasm
use std.os.subprocess (run_capture)

fn _available(str name, list args) bool {
   run_capture(name, args, nil, false).get("code", 127) == 0
}

fn tool_status() dict {
   "Return availability for optional reverse-engineering helpers."
   {
      "capstone": dasm.capstone_available(),
      "assembler": dasm.assembler_available(),
      "objdump": _available("objdump", ["--version"]),
      "readelf": _available("readelf", ["--version"]),
      "cxxfilt": _available("c++filt", ["--version"]),
      "inspiration_note": "clone angr, ghidra, or vex into /tmp only when inspecting upstream implementations",
   }
}

fn demangle(str name) str {
   "Return an Itanium C++ symbol demangled by c++filt when available."
   if !str.startswith(name, "_Z") { return name }
   def r = run_capture("c++filt", [name], nil, false)
   if r.get("code", 1) != 0 { return name }
   def out = str.strip(r.get("stdout", ""))
   out.len > 0 ? out : name
}

#main {
   assert(demangle("plain_name") == "plain_name", "decompiler tools passthrough")
   assert(is_dict(tool_status()), "decompiler tool status")
   print("✓ std.os.rev.decomp.tools self-test passed")
}
