;; Keywords: info environment system-info os
;; OS system information (CPU, RAM, GPU).
;; References:
;; - std.os
module std.os.info(cpu_name, ram_short, gpu_name, hostname, cpu_logical_count, cpu_features_raw, cpu_features, cpu_feature_map, has_cpu_feature, has_opencl, system_info)
use std.core
use std.core.dict_mod
use std.core.str
use std.os
use std.os.fs
use std.os.io
use std.os.path
use std.os.path as ospath
use std.os.platform as platform

mut _linux_cpuinfo_loaded = false
mut _linux_cpuinfo_cache = ""
mut _cpu_features_raw_loaded = false
mut _cpu_features_raw_cache = ""
mut _cpu_features_norm_loaded = false
mut _cpu_features_norm_cache = ""
mut _cpu_features_loaded = false
mut _cpu_features_cache = []
mut _cpu_feature_map_loaded = false
mut _cpu_feature_map_cache = dict(64)
mut _hostname_loaded = false
mut _hostname_cache = ""
mut _cpu_name_loaded = false
mut _cpu_name_cache = ""
mut _cpu_logical_count_loaded = false
mut _cpu_logical_count_cache = 1
mut _ram_short_loaded = false
mut _ram_short_cache = ""
mut _gpu_deep_scan_loaded = false
mut _gpu_deep_scan_cache = false
mut _gpu_scan_cards_loaded = false
mut _gpu_scan_cards_cache = 4
mut _gpu_name_loaded = false
mut _gpu_name_cache = ""
mut _system_info_loaded = false
mut _system_info_cache = dict(32)

fn _linux_cpuinfo_text() str {
   if(!platform.is_linux()){ return "" }
   if(_linux_cpuinfo_loaded){ return _linux_cpuinfo_cache }
   _linux_cpuinfo_cache = _read_text(ospath.normalize("/proc/cpuinfo"))
   _linux_cpuinfo_loaded = true
   _linux_cpuinfo_cache
}

fn _read_text(str path) str {
   match file_read(path){
      ok(s) -> { return s }
      err(ignorederr) -> { ignorederr  return "" }
   }
}

fn _cmd_out(str cmd, list args) str {
   def p = spawn(cmd, args)
   if(!p){ return "" }
   shutdown_send(p)
   mut s = recv_all(p)
   close(p)
   if(!is_str(s)){ return "" }
   s = str_replace(s, "\r", "")
   strip(s)
}

fn _env_bool(str key, bool fallback=false) bool {
   def raw = env(key)
   if(!is_str(raw)){ return fallback }
   def v = lower(strip(raw))
   return case v {
      "1", "true", "yes", "on" -> true
      "0", "false", "no", "off" -> false
      _ -> fallback
   }
}

fn _first_existing_path(list paths) bool {
   mut i = 0
   while(i < paths.len){
      if(file_exists(ospath.normalize(paths.get(i, "")))){ return true }
      i += 1
   }
   false
}

fn _first_line(any s) str {
   if(!is_str(s)){ return "" }
   def lines = split(s, "\n")
   mut i = 0
   while(i < lines.len){
      def ln = strip(lines.get(i, ""))
      if(ln.len > 0){ return ln }
      i += 1
   }
   ""
}

fn _find_value(list lines, str key) str {
   mut i = 0
   while(i < lines.len){
      def ln = strip(lines.get(i, ""))
      if(startswith(ln, key)){
         def idx = find(ln, "=")
         if(idx >= 0){ return strip(ln.slice(idx + 1, ln.len, 1)) }
         return strip(ln)
      }
      i += 1
   }
   ""
}

fn _find_colon_line_value(list lines, str prefix) str {
   mut i = 0
   while(i < lines.len){
      def ln = strip(lines.get(i, ""))
      if(startswith(ln, prefix)){ return _after_colon(ln) }
      i += 1
   }
   ""
}

fn _after_colon(any s) str {
   if(!is_str(s)){ return "" }
   def n = s.len
   mut i = 0
   while(i < n){
      if(load8(s, i) == 58){
         i += 1
         while(i < n && (load8(s, i) == 32 || load8(s, i) == 9)){ i += 1 }
         def out = malloc(n - i + 1)
         if(!out){ return "" }
         init_str(out, n - i)
         mut k = 0
         while(i + k < n){
            store8(out, load8(s, i + k), k)
            k += 1
         }
         store8(out, 0, n - i)
         return strip(out)
      }
      i += 1
   }
   strip(s)
}

fn _first_number(any s) int {
   if(!is_str(s)){ return 0 }
   def n = s.len
   mut i = 0
   while(i < n && (load8(s, i) < 48 || load8(s, i) > 57)){ i += 1 }
   mut v = 0
   while(i < n){
      def c = load8(s, i)
      if(c < 48 || c > 57){ break }
      v = v * 10 + (c - 48)
      i += 1
   }
   v
}

fn _find_prefixed_line_value(any text, any prefix) str {
   if(!is_str(text) || !is_str(prefix)){ return "" }
   def n, m = text.len, prefix.len
   if(n == 0 || m == 0 || n < m){ return "" }
   mut i = 0
   while(i < n){
      def line_start = i
      mut line_end = i
      while(line_end < n && load8(text, line_end) != 10 && load8(text, line_end) != 13){ line_end += 1 }
      mut j = 0
      while(j < m && line_start + j < line_end){
         if(load8(text, line_start + j) != load8(prefix, j)){ break }
         j += 1
      }
      if(j == m){
         mut k = line_start + j
         while(k < line_end){
            if(load8(text, k) == 58){
               k += 1
               break
            }
            k += 1
         }
         while(k < line_end && (load8(text, k) == 32 || load8(text, k) == 9)){ k += 1 }
         if(k < line_end){ return strip(text.slice(k, line_end, 1)) }
         return ""
      }
      i = line_end + 1
      while(i < n && (load8(text, i) == 10 || load8(text, i) == 13)){ i += 1 }
   }
   ""
}

fn _format_mem_usage_kb(int total_kb, int free_kb) str {
   if(total_kb <= 0){ return "" }
   def used_mb = (total_kb - free_kb) / 1024
   def total_mb = total_kb / 1024
   to_str(used_mb) + "/" + to_str(total_mb) + "MB"
}

fn _parse_windows_mem_summary(any text) str {
   if(!is_str(text) || text.len == 0){ return "" }
   def lines = split(text, "\n")
   mut total_kb = 0
   mut free_kb = 0
   mut i = 0
   while(i < lines.len){
      def ln = strip(lines.get(i, ""))
      if(startswith(ln, "TotalVisibleMemorySize")){ total_kb = _first_number(ln) }
      elif(startswith(ln, "FreePhysicalMemory")){ free_kb = _first_number(ln) }
      i += 1
   }
   _format_mem_usage_kb(total_kb, free_kb)
}

fn cpu_logical_count() int {
   "Returns the detected number of logical CPU cores."
   if(_cpu_logical_count_loaded){ return _cpu_logical_count_cache }
   mut out = 0
   if(platform.is_linux()){
      def nproc_out = _cmd_out("nproc", [])
      def n0 = _first_number(nproc_out)
      if(n0 > 0){ out = n0 }
      if(out <= 0){
         def cpu = _linux_cpuinfo_text()
         if(cpu.len > 0){
            def lines = split(cpu, "\n")
            mut n, i = 0, 0
            while(i < lines.len){
               def ln = strip(lines.get(i, ""))
               if(startswith(ln, "processor")){ n += 1 }
               i += 1
            }
            if(n > 0){ out = n }
         }
      }
   } elif(platform.is_macos()){
      def sysctl_out = _cmd_out("sysctl", ["-n", "hw.logicalcpu"])
      def n = _first_number(sysctl_out)
      if(n > 0){ out = n }
      if(out <= 0){
         def ncpu_out = _cmd_out("sysctl", ["-n", "hw.ncpu"])
         def n2 = _first_number(ncpu_out)
         if(n2 > 0){ out = n2 }
      }
   } elif(platform.is_windows()){
      def p, n = env("NUMBER_OF_PROCESSORS"), _first_number(p)
      if(n > 0){ out = n }
      if(out <= 0){
         def wmic_out = _cmd_out("wmic", ["cpu", "get", "NumberOfLogicalProcessors", "/value"])
         def n2 = _first_number(wmic_out)
         if(n2 > 0){ out = n2 }
      }
   }
   if(out <= 0){ out = 1 }
   _cpu_logical_count_cache = out
   _cpu_logical_count_loaded = true
   out
}

fn _linux_cpu_features_raw() str {
   def out = _cmd_out("lscpu", [])
   if(out.len > 0){
      def lines = split(out, "\n")
      mut v2 = _find_colon_line_value(lines, "Flags")
      if(v2.len == 0){ v2 = _find_colon_line_value(lines, "Features") }
      if(v2.len > 0){ return strip(v2) }
   }
   def cpu = _linux_cpuinfo_text()
   if(cpu.len > 0){
      def lines2 = split(cpu, "\n")
      mut i = 0
      while(i < lines2.len){
         def ln = strip(lines2.get(i, ""))
         if(startswith(ln, "flags") || startswith(ln, "Features") || startswith(ln, "isa")){
            def v = _after_colon(ln)
            if(v.len > 0){ return strip(v) }
         }
         i += 1
      }
   }
   ""
}

fn _macos_cpu_features_raw() str {
   mut out = _cmd_out("sysctl", ["-n", "machdep.cpu.features"])
   mut out2 = _cmd_out("sysctl", ["-n", "machdep.cpu.leaf7_features"])
   if(out.len > 0 && out2.len > 0){ return strip(out + " " + out2) }
   if(out.len > 0){ return strip(out) }
   if(out2.len > 0){ return strip(out2) }
   def all = _cmd_out("sysctl", ["hw.optional"])
   if(all.len == 0){ return "" }
   def lines = split(all, "\n")
   mut feats = list(16)
   mut i = 0
   while(i < lines.len){
      def ln = strip(lines.get(i, ""))
      if(startswith(ln, "hw.optional.")){
         def idx = find(ln, ":")
         if(idx >= 0){
            def key = strip(ln.slice(0, idx, 1))
            def val = strip(ln.slice(idx + 1, ln.len, 1))
            if(eq(val, "1")){
               def clean = key.slice(12, key.len, 1)
               feats = feats.append(clean)
            }
         }
      }
      i += 1
   }
   if(feats.len > 0){ return join(feats, " ") }
   ""
}

fn _windows_cpu_features_raw() str {
   def env_cpu = env("PROCESSOR_IDENTIFIER")
   if(is_str(env_cpu) && strip(env_cpu).len > 0){ return strip(env_cpu) }
   mut out = _cmd_out("wmic", ["cpu", "get", "Caption,Name", "/value"])
   if(out.len > 0){ return out }
   out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)"])
   if(out.len > 0){ return _first_line(out) }
   ""
}

fn cpu_features_raw() str {
   "Returns a raw CPU feature string from OS-specific sources."
   if(_cpu_features_raw_loaded){ return _cpu_features_raw_cache }
   mut out = ""
   if(platform.is_linux()){ out = _linux_cpu_features_raw() }
   elif(platform.is_macos()){ out = _macos_cpu_features_raw() }
   elif(platform.is_windows()){ out = _windows_cpu_features_raw() }
   if(!is_str(out)){ out = "" }
   _cpu_features_raw_cache = out
   _cpu_features_raw_loaded = true
   out
}

fn _normalize_feature_text(any raw) str {
   if(!is_str(raw)){ return "" }
   mut s = " " + lower(strip(raw)) + " "
   if(strip(s).len == 0){ return "" }
   def reps = [["\t", " "], ["\n", " "], ["\r", " "], [",", " "], [";", " "], [":", " "], ["=", " "], [".", "_"], ["-", "_"]]
   mut i = 0
   while(i < reps.len){
      def r = reps.get(i)
      s = str_replace(s, r.get(0, ""), r.get(1, ""))
      i += 1
   }
   s
}

fn _cpu_features_norm_text() str {
   if(_cpu_features_norm_loaded){ return _cpu_features_norm_cache }
   _cpu_features_norm_cache = _normalize_feature_text(cpu_features_raw())
   _cpu_features_norm_loaded = true
   _cpu_features_norm_cache
}

fn _feature_has(str norm, str name) bool {
   if(!is_str(norm) || norm.len == 0){ return false }
   str_contains(norm, " " + name + " ")
}

fn _normalize_feature_name(any name) str {
   if(!is_str(name)){ return "" }
   mut s = lower(strip(name))
   if(s.len == 0){ return "" }
   s = str_replace(s, ".", "_")
   s = str_replace(s, "-", "_")
   s = str_replace(s, " ", "_")
   while(str_contains(s, "__")){ s = str_replace(s, "__", "_") }
   if(startswith(s, "_")){ s = s.slice(1, s.len, 1) }
   if(endswith(s, "_")){ s = s.slice(0, s.len - 1, 1) }
   if(s == "sse41"){ return "sse4_1" }
   if(s == "sse42"){ return "sse4_2" }
   if(s == "sha1" || s == "sha2" || s == "sha_ni"){ return "sha" }
   if(s == "asimd"){ return "neon" }
   if(s == "avx512"){ return "avx512f" }
   s
}

fn cpu_features() list {
   "Returns a normalized list of commonly-used CPU feature names."
   if(_cpu_features_loaded){ return clone(_cpu_features_cache) }
   def n = _cpu_features_norm_text()
   mut out = list(32)
   def curated = ["mmx", "sse", "sse2", "sse3", "ssse3", "sse4_1", "sse4_2", "popcnt", "aes", "pclmulqdq", "fma", "avx", "avx2", "avx512f", "bmi1", "bmi2"]
   mut i = 0
   while(i < curated.len){
      def name = curated.get(i, "")
      if(_feature_has(n, name)){ out = out.append(name) }
      i += 1
   }
   if(_feature_has(n, "sha_ni") || _feature_has(n, "sha1") || _feature_has(n, "sha2")){ out = out.append("sha") }
   if(_feature_has(n, "neon") || _feature_has(n, "asimd")){ out = out.append("neon") }
   def tail = ["sve", "crc32", "atomics", "fp16"]
   i = 0
   while(i < tail.len){
      def name = tail.get(i, "")
      if(_feature_has(n, name)){ out = out.append(name) }
      i += 1
   }
   _cpu_features_cache = out
   _cpu_features_loaded = true
   clone(out)
}

fn cpu_feature_map() dict {
   "Returns a normalized dictionary of commonly-used detected CPU features.
   The returned dict maps normalized feature names to `true` and is optimized
   for fast lookup of common features.
   Examples:
   - `\"sse4.1\"`, `\"sse41\"` and `\"sse4_1\"` normalize to `\"sse4_1\"`
   - `\"sha_ni\"`, `\"sha1\"`, `\"sha2\"` normalize to `\"sha\"`
   - `\"asimd\"` normalizes to `\"neon\"`
   "
   if(_cpu_feature_map_loaded){ return dict_clone(_cpu_feature_map_cache) }
   mut m = dict(64)
   def curated = cpu_features()
   mut j = 0
   while(j < curated.len){
      def c = _normalize_feature_name(curated.get(j, ""))
      if(c.len > 0){ m = m.set(c, true) }
      j += 1
   }
   _cpu_feature_map_cache = m
   _cpu_feature_map_loaded = true
   dict_clone(m)
}

fn has_cpu_feature(any name) bool {
   "Returns true when CPU feature `name` is detected.
   `name` is normalized, so variants like `\"sse4.1\"`, `\"sse41\"`,
   and `\"sse4_1\"` are equivalent.
   "
   def n = _normalize_feature_name(name)
   if(n.len == 0){ return false }
   if(cpu_feature_map().get(n, false)){ return true }
   _feature_has(_cpu_features_norm_text(), n)
}

fn _gpu_deep_scan_enabled() bool {
   if(_gpu_deep_scan_loaded){ return _gpu_deep_scan_cache }
   _gpu_deep_scan_cache = _env_bool("NYTRIX_GPU_DEEP_SCAN", false)
   _gpu_deep_scan_loaded = true
   _gpu_deep_scan_cache
}

fn _gpu_scan_cards_limit() int {
   if(_gpu_scan_cards_loaded){ return _gpu_scan_cards_cache }
   mut n = 4
   def raw = env("NYTRIX_GPU_SCAN_CARDS")
   if(is_str(raw)){
      def s = strip(raw)
      if(s.len > 0){
         def v = atoi(s)
         if(v > 0){
            n = v
            if(n > 8){ n = 8 }
         }
      }
   }
   _gpu_scan_cards_cache = n
   _gpu_scan_cards_loaded = true
   n
}

fn has_opencl() bool {
   "Returns true when an OpenCL runtime appears available on this host."
   def force = env("NYTRIX_OPENCL_FORCE")
   if(is_str(force)){ return _env_bool("NYTRIX_OPENCL_FORCE", false) }
   if(platform.is_windows()){
      return _first_existing_path(["C:\\Windows\\System32\\OpenCL.dll", "C:\\Windows\\SysWOW64\\OpenCL.dll"])
   }
   if(platform.is_macos()){ return file_exists(ospath.normalize("/System/Library/Frameworks/OpenCL.framework/OpenCL")) }
   _first_existing_path(["/etc/OpenCL/vendors", "/usr/lib/libOpenCL.so", "/usr/lib64/libOpenCL.so", "/usr/local/lib/libOpenCL.so", "/lib/x86_64-linux-gnu/libOpenCL.so.1", "/usr/lib/x86_64-linux-gnu/libOpenCL.so.1"])
}

fn hostname() str {
   "Returns the machine hostname."
   if(_hostname_loaded){ return _hostname_cache }
   mut out = ""
   if(platform.is_windows()){
      def hn = env("COMPUTERNAME")
      if(is_str(hn) && strip(hn).len > 0){ out = strip(hn) }
      if(out.len == 0){
         def host_out = _cmd_out("hostname", [])
         if(host_out.len > 0){ out = _first_line(host_out) }
      }
      if(out.len == 0){ out = "windows-host" }
      _hostname_cache = out
      _hostname_loaded = true
      return out
   }
   def env_hn = env("HOSTNAME")
   if(is_str(env_hn) && strip(env_hn).len > 0){ out = strip(env_hn) }
   if(out.len == 0 && platform.is_linux()){
      def proc_hn = _read_text(ospath.normalize("/proc/sys/kernel/hostname"))
      if(proc_hn.len > 0){
         def h0 = _first_line(proc_hn)
         if(h0.len > 0){ out = h0 }
      }
   }
   if(out.len == 0 && platform.is_macos()){
      def sys_hn = _cmd_out("sysctl", ["-n", "kern.hostname"])
      if(sys_hn.len > 0){
         def h1 = _first_line(sys_hn)
         if(h1.len > 0){ out = h1 }
      }
   }
   if(out.len == 0){
      def etc_hn = _read_text(ospath.normalize("/etc/hostname"))
      if(etc_hn.len > 0){
         def h = _first_line(etc_hn)
         if(h.len > 0){ out = h }
      }
   }
   if(out.len == 0 && !platform.is_macos()){
      def host_out = _cmd_out("hostname", [])
      if(host_out.len > 0){ out = _first_line(host_out) }
   }
   if(out.len == 0){ out = os() + "-host" }
   _hostname_cache = out
   _hostname_loaded = true
   out
}

fn cpu_name() str {
   "Returns the CPU model name.
   Linux uses /proc, macOS uses sysctl, Windows uses wmic/powershell."
   if(_cpu_name_loaded){ return _cpu_name_cache }
   mut out = ""
   if(load8(__os_name(), 0) == 108){
      def ls = _cmd_out("lscpu", [])
      if(ls.len > 0){
         def lines = split(ls, "\n")
         def lm = _find_colon_line_value(lines, "Model name")
         if(lm.len > 0){ out = lm }
         if(out.len == 0){
            def la = _find_colon_line_value(lines, "Architecture")
            if(la.len > 0){ out = la }
         }
      }
      if(out.len == 0){
         def cpu = _linux_cpuinfo_text()
         if(cpu.len > 0){
            def lines2 = split(cpu, "\n")
            mut i = 0
            while(i < lines2.len){
               def ln = strip(lines2.get(i, ""))
               if(startswith(ln, "model name") || startswith(ln, "Hardware")){
                  def v = _after_colon(ln)
                  if(v.len > 0){
                     out = v
                     break
                  }
               }
               i += 1
            }
            if(out.len == 0){ out = _first_line(cpu) }
         }
      }
   } elif(load8(__os_name(), 0) == 109){
      mut mac_out = _cmd_out("sysctl", ["-n", "machdep.cpu.brand_string"])
      if(mac_out.len > 0){ out = _first_line(mac_out) }
      if(out.len == 0){
         mac_out = _cmd_out("sysctl", ["-n", "hw.model"])
         if(mac_out.len > 0){ out = _first_line(mac_out) }
      }
   } elif(load8(__os_name(), 0) == 119){
      mut win_out = _cmd_out("wmic", ["cpu", "get", "Name", "/value"])
      if(win_out.len > 0){
         def lines = split(win_out, "\n")
         def v = _find_value(lines, "Name")
         if(v.len > 0){ out = v }
      }
      if(out.len == 0){
         win_out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)"])
         if(win_out.len > 0){ out = _first_line(win_out) }
      }
      if(out.len == 0){
         def env_cpu = env("PROCESSOR_IDENTIFIER")
         if(is_str(env_cpu) && env_cpu.len > 0){ out = env_cpu }
      }
   }
   if(out.len == 0){ out = os() + " cpu" }
   _cpu_name_cache = out
   _cpu_name_loaded = true
   out
}

fn ram_short() str {
   "Returns a summary string of system RAM usage(e.g., '2048/8192MB').
   Linux uses /proc ; macOS uses sysctl/vm_stat; Windows uses wmic/powershell."
   if(_ram_short_loaded){ return _ram_short_cache }
   if(platform.is_linux()){
      def mem = _read_text(ospath.normalize("/proc/meminfo"))
      if(mem.len > 0){
         def total_v = _find_prefixed_line_value(mem, "MemTotal")
         def avail_v = _find_prefixed_line_value(mem, "MemAvailable")
         def total_kb = _first_number(total_v)
         def avail_kb = _first_number(avail_v)
         if(total_kb > 0){
            if(avail_kb <= 0){
               _ram_short_cache = to_str(total_kb / 1024) + "MB total"
               _ram_short_loaded = true
               return _ram_short_cache
            }
            _ram_short_cache = _format_mem_usage_kb(total_kb, avail_kb)
            _ram_short_loaded = true
            return _ram_short_cache
         }
      }
   } elif(platform.is_macos()){
      def total = _cmd_out("sysctl", ["-n", "hw.memsize"])
      mut total_b = _first_number(total)
      if(total_b <= 0){
         def t2 = _cmd_out("sysctl", ["-n", "hw.physmem"])
         total_b = _first_number(t2)
      }
      def vm = _cmd_out("vm_stat", [])
      if(total_b > 0 && vm.len > 0){
         def lines = split(vm, "\n")
         mut page_sz = 4096
         mut free_p = 0
         mut spec_p = 0
         mut i = 0
         while(i < lines.len){
            def ln = strip(lines.get(i, ""))
            if(startswith(ln, "Mach Virtual Memory Statistics")){
               page_sz = _first_number(ln)
               if(page_sz <= 0){ page_sz = 4096 }
            } elif(startswith(ln, "Pages free:")){
               free_p = _first_number(ln)
            } elif(startswith(ln, "Pages speculative:")){
               spec_p = _first_number(ln)
            }
            i += 1
         }
         def free_b = (free_p + spec_p) * page_sz
         mut used_b = total_b - free_b
         if(used_b < 0){ used_b = 0 }
         def used_mb = used_b / (1024 * 1024)
         def total_mb = total_b / (1024 * 1024)
         _ram_short_cache = to_str(used_mb) + "/" + to_str(total_mb) + "MB"
         _ram_short_loaded = true
         return _ram_short_cache
      }
      if(total_b > 0){
         def total_mb = total_b / (1024 * 1024)
         _ram_short_cache = to_str(total_mb) + "MB total"
         _ram_short_loaded = true
         return _ram_short_cache
      }
   } elif(platform.is_windows()){
      mut out = _cmd_out("wmic", ["OS", "get", "TotalVisibleMemorySize,FreePhysicalMemory", "/value"])
      def wmic_mem = _parse_windows_mem_summary(out)
      if(wmic_mem.len > 0){
         _ram_short_cache = wmic_mem
         _ram_short_loaded = true
         return _ram_short_cache
      }
      out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_OperatingSystem | Select-Object -First 1 TotalVisibleMemorySize,FreePhysicalMemory | Format-List)"])
      def ps_mem = _parse_windows_mem_summary(out)
      if(ps_mem.len > 0){
         _ram_short_cache = ps_mem
         _ram_short_loaded = true
         return _ram_short_cache
      }
   }
   _ram_short_cache = os() + " ram"
   _ram_short_loaded = true
   _ram_short_cache
}

fn gpu_name() str {
   "Returns the name or primary identifier of the system's GPU.
   Linux uses sysfs ; macOS uses system_profiler (cached); Windows uses wmic/powershell (cached)."
   if(_gpu_name_loaded){ return _gpu_name_cache }
   if(platform.is_macos()){
      def soc = _cmd_out("sysctl", ["-n", "machdep.cpu.brand_string"])
      if(str_contains(soc, "Apple M")){
         _gpu_name_cache = "Apple GPU(" + strip(soc) + ")"
         _gpu_name_loaded = true
         return _gpu_name_cache
      }
      if(_gpu_deep_scan_enabled()){
         def out = _cmd_out("system_profiler", ["SPDisplaysDataType", "-detailLevel", "mini"])
         if(out.len > 0){
            def lines = split(out, "\n")
            mut i = 0
            while(i < lines.len){
               def ln = strip(lines.get(i, ""))
               if(startswith(ln, "Chipset Model:")){
                  _gpu_name_cache = _after_colon(ln)
                  _gpu_name_loaded = true
                  return _gpu_name_cache
               }
               if(startswith(ln, "Model:")){
                  _gpu_name_cache = _after_colon(ln)
                  _gpu_name_loaded = true
                  return _gpu_name_cache
               }
               i += 1
            }
         }
      }
      _gpu_name_cache = "macos gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   if(platform.is_windows()){
      if(_gpu_deep_scan_enabled()){
         mut out = _cmd_out("wmic", ["path", "win32_VideoController", "get", "Name", "/value"])
         if(out.len > 0){
            def lines = split(out, "\n")
            def v = _find_value(lines, "Name")
            if(v.len > 0){
               _gpu_name_cache = v
               _gpu_name_loaded = true
               return _gpu_name_cache
            }
         }
         out = _cmd_out("powershell", ["-NoProfile", "-Command", "(Get-CimInstance Win32_VideoController | Select-Object -First 1 -ExpandProperty Name)"])
         if(out.len > 0){
            _gpu_name_cache = _first_line(out)
            _gpu_name_loaded = true
            return _gpu_name_cache
         }
      }
      _gpu_name_cache = "windows gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   if(load8(__os_name(), 0) != 108){
      _gpu_name_cache = __os_name() + " gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   def max_cards = _gpu_scan_cards_limit()
   mut i = 0
   while(i < max_cards){
      def base = ospath.normalize("/sys/class/drm/card" + to_str(i) + "/device/")
      def vendor = strip(_read_text(base + "vendor"))
      def dev = strip(_read_text(base + "device"))
      if(vendor.len > 0 && dev.len > 0){
         _gpu_name_cache = "pci " + vendor + ":" + dev
         _gpu_name_loaded = true
         return _gpu_name_cache
      }
      i += 1
   }
   if(!_gpu_deep_scan_enabled()){
      _gpu_name_cache = os() + " gpu"
      _gpu_name_loaded = true
      return _gpu_name_cache
   }
   def pci_path = ospath.normalize("/sys/bus/pci/devices")
   if(file_exists(pci_path)){
      def entries = list_dir(pci_path)
      mut j = 0
      while(j < entries.len){
         def entry = entries.get(j, "")
         def class_file = ospath.normalize(pci_path + "/" + entry + "/class")
         if(file_exists(class_file)){
            def class_hex = strip(_read_text(class_file))
            if(startswith(class_hex, "0x0300") || startswith(class_hex, "0x0302")){
               def vf = ospath.normalize(pci_path + "/" + entry + "/vendor")
               def df = ospath.normalize(pci_path + "/" + entry + "/device")
               if(file_exists(vf) && file_exists(df)){
                  def v, d = strip(_read_text(vf)), strip(_read_text(df))
                  if(v.len > 0 && d.len > 0){
                     _gpu_name_cache = "pci " + v + ":" + d
                     _gpu_name_loaded = true
                     return _gpu_name_cache
                  }
               }
            }
         }
         j += 1
      }
   }
   _gpu_name_cache = os() + " gpu"
   _gpu_name_loaded = true
   _gpu_name_cache
}

fn system_info() dict {
   "Returns a dictionary with common host information.
   GPU/accelerator/parallel policy fields reflect current runtime configuration.
   In CLI usage, compiler flags are the primary source(for example `--gpu`,
   `--accel-target`, `--parallel`)."
   if(_system_info_loaded){ return dict_clone(_system_info_cache) }
   mut logical = cpu_logical_count()
   if(logical <= 0){ logical = 1 }
   def gst = gpu_offload_status(0)
   def ast = accel_target_status()
   def pst = parallel_status(0)
   def d = {
      "os": os(), "arch": arch(), "platform": os() + "/" + arch(),
      "hostname": hostname(), "cpu": cpu_name(), "logical_cpus": logical,
      "cpu_features_raw": cpu_features_raw(), "cpu_features": cpu_features(),
      "cpu_feature_map": cpu_feature_map(), "ram": ram_short(), "gpu": gpu_name(),
      "opencl": has_opencl(), "gpu_mode": gpu_mode(), "gpu_backend": gpu_backend(),
      "gpu_offload": gpu_offload(), "gpu_min_work": gpu_min_work(), "gpu_async": gpu_async(),
      "gpu_fast_math": gpu_fast_math(), "gpu_available": gpu_available(),
      "gpu_selected_backend": gst.get("selected_backend", gpu_backend()),
      "gpu_offload_policy_selected": gst.get("policy_selected", false),
      "gpu_offload_active": gst.get("active", false),
      "gpu_offload_reason": gst.get("reason", ""),
      "gpu_offload_active_reason": gst.get("active_reason", ""),
      "accel_target": accel_target(), "accel_targets": accel_targets(),
      "accel_binary_kind": accel_binary_kind(), "accel_binary_ext": accel_binary_ext(),
      "accel_status": ast, "accel_available": ast.get("available", false),
      "parallel_mode": parallel_mode(), "parallel_threads": parallel_threads(),
      "parallel_min_work": parallel_min_work(),
      "parallel_effective_threads": pst.get("effective_threads", parallel_threads()),
      "parallel_policy_selected": pst.get("selected", false),
      "parallel_reason": pst.get("reason", ""),
      "hardware_threads": hardware_threads(), "opencl_available": opencl_available(),
      "opencl_toolchain_available": opencl_toolchain_available(), "opencl_status": opencl_status(0),
   }
   _system_info_cache = d
   _system_info_loaded = true
   dict_clone(d)
}

#main {
   assert(cpu_name().len > 0 && ram_short().len > 0 && hostname().len >= 0, "info host strings")
   assert(cpu_logical_count() >= 1, "info cpu logical count")
   assert(is_list(cpu_features()) && is_dict(cpu_feature_map()), "info cpu features")
   assert(is_bool(has_cpu_feature("sse2")) && is_bool(has_opencl()), "info feature booleans")
   def info = system_info()
   assert(is_dict(info) && info.get("os", "") == os() && info.get("arch", "") == arch(), "info system os arch")
   assert(info.get("logical_cpus", 0) >= 1 && info.get("platform", "").len > 0, "info system basics")
   assert(info.get("gpu", "").len > 0 && info.get("ram", "").len > 0, "info system hardware strings")
   assert(is_dict(info.get("accel_status", 0)) && is_dict(info.get("opencl_status", 0)), "info accel/opencl status")
   print("✓ std.os.info self-test passed")
}
