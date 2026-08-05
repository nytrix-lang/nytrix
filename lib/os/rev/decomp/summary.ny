;; Keywords: decompiler semantic summary findings analyst
;; Data-only summary helpers shared by the public decompiler facade.
module std.os.rev.decomp.summary *

use std.core
use std.core.str as str
use std.os.rev.decomp.elf

fn _summary_hex(int value) str {
   "0x" + str.to_hex(value, 0)
}

fn _summary_is_input_import(str name0) bool {
   def name = str.lower(name0)
   name == "read" || name == "_read" || name == "readv" || name == "_readv" ||
   name == "gets" || name == "fgets" || name == "getline" || name == "scanf" ||
   name == "__isoc99_scanf" || name == "__isoc23_scanf" || name == "fscanf" ||
   name == "sscanf" || name == "getchar" || name == "getc" || name == "fgetc"
}

fn _summary_import_category(str name) str {
   def n = str.lower(_display_symbol(name))
   if _summary_is_input_import(n) { return "input" }
   if (n == "ptrace" || n == "prctl" || n == "sysctl" || n == "uname" ||
      n == "getauxval" || n == "dlopen" || n == "dlsym" || n == "isdebuggerpresent" ||
   str.find(n, "debug") >= 0){ return "anti_analysis" }
   if (n == "strcmp" || n == "strncmp" || n == "memcmp" || n == "memchr" || n == "strstr" ||
   n == "strlen" || n == "atoi" || n == "strtol" || n == "sscanf"){ return "compare_parse" }
   if n == "memcpy" || n == "memmove" || n == "memset" || n == "memfrob" { return "memory_transform" }
   if n == "uncompress" || n == "compress" || n == "crc32" || str.find(n, "zlib") >= 0 { return "compression" }
   if (str.find(n, "crypto") >= 0 || str.find(n, "aes") >= 0 || str.find(n, "sha") >= 0 ||
   str.find(n, "md5") >= 0 || str.find(n, "evp") >= 0){ return "crypto" }
   if (n == "open" || n == "openat" || n == "read" || n == "write" || n == "close" ||
   n == "stat" || n == "fstat" || n == "mmap" || n == "ioctl"){ return "os" }
   "other"
}

fn _summary_import_records(dict bin) list {
   mut out = []
   mut seen = dict()
   def imps = bin.contains("imports") ? bin.get("imports", []) : imports(bin)
   mut i = 0
   while i < imps.len {
      def item = imps[i]
      def name = _display_symbol(is_dict(item) ? item.get("name", item.get("symbol", "")) : to_str(item))
      if name.len > 0 && !seen.get(name, false) {
         seen = seen.set(name, true)
         out = out.append({"name": name, "category": _summary_import_category(name)})
      }
      i += 1
   }
   def sites = bin.contains("import_sites") ? bin.get("import_sites", []) : import_sites(bin)
   i = 0
   while i < sites.len {
      def name = _display_symbol(sites[i].get("display_name", sites[i].get("name", sites[i].get("symbol", ""))))
      if name.len > 0 && !seen.get(name, false) {
         seen = seen.set(name, true)
         out = out.append({"name": name, "category": _summary_import_category(name)})
      }
      i += 1
   }
   out
}

fn _summary_count_categories(list records) dict {
   mut out = dict()
   mut i = 0
   while i < records.len {
      def category = records[i].get("category", "other")
      out = out.set(category, int(out.get(category, 0)) + 1)
      i += 1
   }
   out
}

fn _summary_add_finding(list xs, str severity, str kind, str message, any evidence=dict()) list {
   xs = xs.append({"severity": severity, "kind": kind, "message": message, "evidence": evidence})
   xs
}

fn _summary_call_records(list calls) list {
   mut out = []
   mut i = 0
   while i < calls.len {
      def call = calls[i]
      def name = _display_symbol(call.get("name", ""))
      if name.len > 0 {
         def category = _summary_import_category(name)
         if category != "other" {
            out = out.append({"name": name, "category": category, "addr": int(call.get("addr", 0)),
                  "target": int(call.get("target", 0)), "argc": int(call.get("argc", 0)),
            "effect": call.get("effect", "")})
         }
      }
      i += 1
   }
   out
}

fn _summary_syscall_records(list syscalls) list {
   mut out = []
   mut i = 0
   while i < syscalls.len {
      def syscall = syscalls[i]
      def name = syscall.get("name", "")
      out = out.append({"name": name, "category": _summary_import_category(name), "addr": int(syscall.get("addr", 0)),
      "nr": int(syscall.get("nr", -1)), "effect": syscall.get("effect", "")})
      i += 1
   }
   out
}

fn _summary_input_surfaces(dict model, list notable_calls, list syscalls) list {
   mut out = []
   mut i = 0
   while i < notable_calls.len {
      def call = notable_calls[i]
      if call.get("category", "") == "input" {
         out = out.append({"kind": "call", "name": call.get("name", ""), "addr": int(call.get("addr", 0)),
         "effect": call.get("effect", "")})
      }
      i += 1
   }
   i = 0
   while i < syscalls.len {
      def syscall = syscalls[i]
      if syscall.get("name", "") == "read" || syscall.get("name", "") == "readv" || syscall.get("name", "") == "recvfrom" {
         out = out.append({"kind": "syscall", "name": syscall.get("name", ""), "addr": int(syscall.get("addr", 0)),
         "effect": syscall.get("effect", "")})
      }
      i += 1
   }
   def params = model.get("signature", dict()).get("params", [])
   i = 0
   while i < params.len {
      def param = params[i]
      if (param.get("shape", "") == "buffer" || param.get("shape", "") == "struct_ptr" ||
         param.get("shape", "") == "pointer" || param.get("reg", "").len > 0){
         out = out.append({"kind": "abi_param", "name": param.get("name", ""), "reg": param.get("reg", ""),
         "shape": param.get("shape", ""), "fields": param.get("fields", [])})
      }
      i += 1
   }
   out
}

fn _summary_hard_branches(dict facts0, int limit=12) list {
   def deps = facts0.get("branch_dependencies", [])
   mut out = []
   mut i = 0
   while i < deps.len && out.len < limit {
      def dep = deps[i]
      out = out.append({"addr": int(dep.get("addr", 0)), "addr_hex": _summary_hex(int(dep.get("addr", 0))),
            "target": int(dep.get("target", 0)), "target_hex": _summary_hex(int(dep.get("target", 0))),
            "condition": dep.get("condition", ""), "expr": dep.get("expr", ""),
            "feeder_row": int(dep.get("feeder_row", -1)),
      "feeder_mnemonic": dep.get("feeder_mnemonic", "")})
      i += 1
   }
   out
}

fn _summary_memory_shapes(dict facts0, int limit=12) list {
   def shapes = facts0.get("memory_shapes", [])
   mut out = []
   mut i = 0
   while i < shapes.len && out.len < limit {
      def shape = shapes[i]
      if int(shape.get("access_count", 0)) > 0 {
         out = out.append({"kind": shape.get("kind", ""), "base": shape.get("base", ""),
               "index": shape.get("index", ""), "scale": int(shape.get("scale", 0)),
               "offsets": shape.get("offsets", []), "widths": shape.get("widths", []),
               "stride": int(shape.get("stride", 0)), "span": int(shape.get("span", 0)),
               "reads": int(shape.get("read_count", 0)), "writes": int(shape.get("write_count", 0)),
         "accesses": int(shape.get("access_count", 0))})
      }
      i += 1
   }
   out
}

fn _summary_state_machines(dict model, int limit=8) list {
   def machines = model.get("state_machines", [])
   mut out = []
   mut i = 0
   while i < machines.len && out.len < limit {
      def machine = machines[i]
      if int(machine.get("case_count", 0)) >= 2 {
         out = out.append({"kind": "dispatcher_state_machine",
               "selector": machine.get("selector", ""), "switch": int(machine.get("switch_addr", 0)),
               "switch_hex": _summary_hex(int(machine.get("switch_addr", 0))),
               "case_count": int(machine.get("case_count", 0)),
               "transition_count": int(machine.get("transition_count", 0)),
               "table": int(machine.get("table", 0)), "table_hex": _summary_hex(int(machine.get("table", 0))),
         "cases": machine.get("cases", [])})
      }
      i += 1
   }
   if out.len > 0 { return out }
   def tables = model.get("jump_tables", [])
   def loop_count = model.get("loops", dict()).get("loops", []).len
   i = 0
   while i < tables.len && out.len < limit {
      def table = tables[i]
      def count = int(table.get("count", table.get("entries", []).len))
      if count >= 4 || (count >= 2 && loop_count > 0) {
         out = out.append({"kind": "dispatcher_candidate",
               "from": int(table.get("from", 0)), "from_hex": _summary_hex(int(table.get("from", 0))),
               "base": int(table.get("base", 0)), "base_hex": _summary_hex(int(table.get("base", 0))),
               "selector": table.get("index_reg", ""), "case_count": count,
         "relative": table.get("relative", false), "loop_count": loop_count})
      }
      i += 1
   }
   out
}

fn _summary_recommendations(dict summary) list {
   mut out = []
   if summary.get("crackme_success_count", 0) > 0 { out = out.append("run crackme_report(...) to derive the concrete model and patch plan") }
   if summary.get("input_surface_count", 0) > 0 && summary.get("branch_count", 0) > 0 { out = out.append("run symbolic_solve(..., {engine:'lifted'}) against success/failure predicates") }
   if summary.get("hard_branch_count", 0) > 0 { out = out.append("inspect hard_branches or path_explain(...) before trusting rendered control flow") }
   if summary.get("memory_shape_count", 0) > 0 { out = out.append("use memory_shapes/vsa intervals to rename struct fields before decompiling") }
   if summary.get("semantic_simplification_count", 0) > 0 { out = out.append("inspect semantic_simplifications for proof-backed expression collapses") }
   if summary.get("state_machine_count", 0) > 0 { out = out.append("inspect state_machines and slice the dispatcher selector before flattening control flow") }
   if summary.get("loop_count", 0) > 0 { out = out.append("prefer sliced symbolic solving around loop exit branches") }
   if summary.get("anti_analysis_count", 0) > 0 { out = out.append("install hooks for anti-analysis imports/syscalls before executing paths") }
   if out.len == 0 { out = out.append("inspect function_model(...) and facts(...) for low-level evidence") }
   out
}

fn _summary_score(dict summary) int {
   int(summary.get("row_count", 0)) + int(summary.get("branch_count", 0)) * 3 +
   int(summary.get("hard_branch_count", 0)) * 4 + int(summary.get("call_count", 0)) * 2 +
   int(summary.get("input_surface_count", 0)) * 6 + int(summary.get("anti_analysis_count", 0)) * 8 +
   int(summary.get("memory_shape_count", 0)) * 3 + int(summary.get("semantic_simplification_count", 0)) * 3 +
   int(summary.get("state_machine_count", 0)) * 10 + int(summary.get("loop_count", 0)) * 4 +
   int(summary.get("crackme_success_count", 0)) * 12 + int(summary.get("finding_count", 0)) * 2
}

fn _summary_text(dict summary) str {
   mut lines = ["semantic summary " + summary.get("name", "") + " " + summary.get("arch", "") +
      " rows=" + to_str(summary.get("row_count", 0)) + " branches=" + to_str(summary.get("branch_count", 0)) +
   " calls=" + to_str(summary.get("call_count", 0)) + " score=" + to_str(summary.get("score", 0))]
   if summary.get("input_surface_count", 0) > 0 { lines = lines.append("input surfaces: " + to_str(summary.get("input_surface_count", 0))) }
   if summary.get("memory_shape_count", 0) > 0 { lines = lines.append("memory shapes: " + to_str(summary.get("memory_shape_count", 0))) }
   if summary.get("semantic_simplification_count", 0) > 0 { lines = lines.append("proved rewrites: " + to_str(summary.get("semantic_simplification_count", 0))) }
   if summary.get("state_machine_count", 0) > 0 { lines = lines.append("state machines: " + to_str(summary.get("state_machine_count", 0))) }
   if summary.get("hard_branch_count", 0) > 0 { lines = lines.append("hard branches: " + to_str(summary.get("hard_branch_count", 0))) }
   if summary.get("anti_analysis_count", 0) > 0 { lines = lines.append("anti-analysis: " + to_str(summary.get("anti_analysis_count", 0))) }
   if summary.get("crackme_success_count", 0) > 0 { lines = lines.append("crackme success targets: " + to_str(summary.get("crackme_success_count", 0))) }
   def recommendations = summary.get("recommendations", [])
   mut i = 0
   while i < recommendations.len { lines = lines.append("next: " + recommendations[i]) i += 1 }
   str.join(lines, "\n")
}

#main {
   def records = _summary_import_records({"imports": [{"name": "read"}, {"name": "ptrace"}]})
   assert(_summary_count_categories(records).get("input", 0) == 1, "summary input category")
   assert(_summary_count_categories(records).get("anti_analysis", 0) == 1, "summary anti-analysis category")
   assert(_summary_hard_branches({"branch_dependencies": [{"addr": 16, "target": 32}]}).get(0).get("addr_hex", "") == "0x10", "summary branch hex")
   print("✓ std.os.rev.decomp.summary self-test passed")
}
