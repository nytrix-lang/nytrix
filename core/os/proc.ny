;; Keywords: os proc sysfs linux processes cpu memory network disk sensors mounts pressure
;; Linux /proc and /sys readers: bounded file reads, `key: value` tables,
;; per-process stat records, CPU/network/disk counters, memory, users,
;; hwmon sensors, mounts with capacity, DRM GPUs, PSI pressure, readlink and
;; pidfd signals. Every reader returns an empty value (or -1) where the file
;; or syscall is unavailable, so callers can poll without error handling.
;; References:
;; - core.os.info
;; - https://docs.kernel.org/filesystems/proc.html
module core.os.proc(read, kv, kv_int, pid_stat, pid_stat_valid, cpu_times, net_counters, disk_counters, meminfo, users, sensors, fs_capacity, mounts, mount_unescape, gpus, pressure, readlink, pidfd_open, pidfd_signal)
use core
use core.str as st
use core.os.fs as fs
use core.os.sys as sys

fn _num(any x) int { st.atoi(to_str(x)) }

fn _field(list a, int i) int { _num(a.get(i, "0")) }

fn read(str path, int limit=65536) str {
   "Reads at most `limit` bytes of `path` (works for /proc files that report
   size 0) ; returns \"\" when it cannot be opened."
   if limit <= 0 { return "" }
   def opened = sys.sys_open(path, 0, 0)
   if !is_ok(opened) { return "" }
   def int fd = unwrap(opened)
   defer { sys.sys_close_quiet(fd) }
   def ptr buf = bytes_new_raw(limit + 1)
   if !buf { return "" }
   mut int n = 0
   while n < limit {
      def r = sys.sys_read(fd, ptr_add(buf, n), limit - n)
      if !is_ok(r) { break }
      def int got = unwrap(r)
      if got <= 0 { break }
      n += got
   }
   init_str(buf, n)
}

fn kv(str text) dict {
   "Parses `key: value` lines(/proc/meminfo, /proc/PID/status) into a dict
   of stripped strings."
   mut out = {}
   def lines = st.split(text, "\n")
   mut int i = 0
   while i < lines.len {
      def line = to_str(lines.get(i, ""))
      def p = st.find(line, ":")
      if p > 0 {
         out = out.set(st.strip(slice(line, 0, p)), st.strip(slice(line, p + 1, line.len)))
      }
      i += 1
   }
   out
}

fn kv_int(str text, str key, int fallback=-1) int {
   "Returns the leading decimal number after `key:` in `key: value` text,
   without building a table ; `fallback` when the key or number is missing."
   def str prefix = key + ":"
   mut int pos = 0
   if st.find(text, prefix) != 0 {
      pos = st.find(text, "\n" + prefix)
      if pos < 0 { return fallback }
      pos += 1
   }
   pos += prefix.len
   while pos < text.len && (load8(text, pos) == 32 || load8(text, pos) == 9) { pos += 1 }
   mut int n = 0, count = 0
   while pos < text.len {
      def int c = load8(text, pos)
      if c < 48 || c > 57 { break }
      n = n * 10 + c - 48
      pos += 1
      count += 1
   }
   count > 0 ? n : fallback
}

fn pid_stat(str line) list {
   "Parses one /proc/PID/stat line into
   [pid, comm, state, ppid, cpu_ticks(utime+stime), nice, threads,
   start_ticks, vsize_bytes, last_cpu] ; [] when malformed. The command name may
   contain spaces and parentheses, so it is cut at the last ')'."
   def a = st.find(line, "("), b = st.find_last(line, ")")
   if a < 0 || b <= a || b + 1 >= line.len { return [] }
   def f = st.split_words(slice(line, b + 1, line.len))
   if f.len < 22 { return [] }
   [_num(st.strip(slice(line, 0, a))), slice(line, a + 1, b), f.get(0, "?"), _field(f, 1), _field(f, 11) + _field(f, 12), _field(f, 16), _field(f, 17), _field(f, 19), _field(f, 20), f.len > 36 ? _field(f, 36) : -1]
}

fn pid_stat_valid(int pid, int start_ticks) bool {
   "True while `pid` still names the process that started at `start_ticks`
   (guards against PID reuse before acting on a process)."
   if pid <= 0 || start_ticks < 0 { return false }
   def row = pid_stat(read("/proc/" + to_str(pid) + "/stat", 4096))
   row.len == 10 && int(row.get(7, -1)) == start_ticks
}

fn cpu_times() dict {
   "Returns {\"cpu\" | \"cpuN\": [total_ticks, idle_ticks]} from /proc/stat
   (idle includes iowait)."
   mut out = {}
   def lines = st.split(read("/proc/stat"), "\n")
   mut int i = 0
   while i < lines.len {
      def a = st.split_words(to_str(lines.get(i, "")))
      def name = to_str(a.get(0, ""))
      if st.find(name, "cpu") == 0 && a.len >= 5 {
         mut total = 0
         mut j = 1
         while j <= 8 && j < a.len { total += _field(a, j) j += 1 }
         out = out.set(name, [total, _field(a, 4) + _field(a, 5)])
      }
      i += 1
   }
   out
}

fn net_counters() dict {
   "Returns {interface: [rx_bytes, tx_bytes]} from /proc/net/dev."
   mut out = {}
   def lines = st.split(read("/proc/net/dev"), "\n")
   mut int i = 0
   while i < lines.len {
      def line = to_str(lines.get(i, ""))
      def p = st.find(line, ":")
      if p > 0 {
         def a = st.split_words(slice(line, p + 1, line.len))
         if a.len >= 16 { out = out.set(st.strip(slice(line, 0, p)), [_field(a, 0), _field(a, 8)]) }
      }
      i += 1
   }
   out
}

fn disk_counters() dict {
   "Returns {device: [read_bytes, written_bytes, io_ms]} for whole block
   devices in /proc/diskstats(partitions, loop and ram devices skipped)."
   mut out = {}
   def devices = fs.list_dir("/sys/block")
   def lines = st.split(read("/proc/diskstats"), "\n")
   mut int i = 0
   while i < lines.len {
      def a = st.split_words(to_str(lines.get(i, "")))
      def name = to_str(a.get(2, ""))
      if a.len >= 14 && devices.contains(name) && st.find(name, "loop") != 0 && st.find(name, "ram") != 0 {
         out = out.set(name, [_field(a, 5) * 512, _field(a, 9) * 512, _field(a, 12)])
      }
      i += 1
   }
   out
}

fn meminfo() dict {
   "Returns /proc/meminfo as {key: \"value unit\"}; see kv_int for numbers."
   kv(read("/proc/meminfo"))
}

fn users() dict {
   "Returns {uid: name} from /etc/passwd."
   mut out = {}
   def lines = st.split(read("/etc/passwd", 1048576), "\n")
   mut int i = 0
   while i < lines.len {
      def a = st.split(to_str(lines.get(i, "")), ":")
      if a.len >= 3 { out = out.set(_field(a, 2), a.get(0, "?")) }
      i += 1
   }
   out
}

fn sensors() list {
   "Returns [label, reading] rows for hwmon temperatures(C), fans(RPM),
   power(W) and batteries(capacity and status)."
   mut rows = []
   def hw = fs.list_dir("/sys/class/hwmon")
   mut int i = 0
   while i < hw.len {
      def base = "/sys/class/hwmon/" + to_str(hw.get(i, "")) + "/"
      def name = st.strip(read(base + "name"))
      def files = fs.list_dir(base)
      mut int j = 0
      while j < files.len {
         def f = to_str(files.get(j, ""))
         def p = st.find(f, "_input")
         if p > 0 && p + 6 == f.len && (st.find(f, "temp") == 0 || st.find(f, "fan") == 0 || st.find(f, "power") == 0) {
            def label = st.strip(read(base + to_str(slice(f, 0, p)) + "_label"))
            def temp = st.find(f, "temp") == 0
            def power = st.find(f, "power") == 0
            def raw = st.strip(read(base + f))
            if raw.len > 0 {
               def v = float(_num(raw)) / (temp ? 1000.0 : (power ? 1000000.0 : 1.0))
               rows = rows.append([name + " / " + (label.len > 0 ? label : slice(f, 0, p)), to_str(round(v, 1)) + (temp ? " C" : (power ? " W" : " RPM"))])
            }
         }
         j += 1
      }
      i += 1
   }
   def supplies = fs.list_dir("/sys/class/power_supply")
   i = 0
   while i < supplies.len {
      def b = to_str(supplies.get(i, ""))
      def path = "/sys/class/power_supply/" + b + "/"
      def capacity = st.strip(read(path + "capacity"))
      if capacity.len > 0 {
         rows = rows.append([b + " battery", capacity + "% " + st.strip(read(path + "status"))])
      }
      i += 1
   }
   rows
}

fn fs_capacity(str path) list {
   "Returns [total_bytes, available_bytes] of the filesystem holding `path`
   (statfs) ; [-1, -1] when unavailable."
   #if x86_64 || aarch64 {
      def ptr buf = malloc(256)
      if !buf { return [-1, -1] }
      defer { free(buf) }
      mut call = 137
      #aarch64 { call = 43 } #endif
      if sys.syscall(call, path, buf) != 0 { return [-1, -1] }
      def int unit = load64_i(buf, 72)
      def int block = unit > 0 ? unit : load64_i(buf, 8)
      def int total = load64_i(buf, 16) * block
      def int available = load64_i(buf, 32) * block
      return [total, available]
   } #else { return [-1, -1] } #endif
}

fn mount_unescape(str x) str {
   "Decodes the octal escapes /proc/self/mountinfo uses in paths."
   def a = st.str_replace(x, "\\040", " ")
   def b = st.str_replace(a, "\\011", "\t")
   def c = st.str_replace(b, "\\012", "\n")
   st.str_replace(c, "\\134", "\\")
}

fn mounts(list kinds=["ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "zfs", "vfat", "exfat", "ntfs3", "overlay", "bcachefs", "tmpfs"]) list {
   "Returns [path, fstype, total_bytes, available_bytes] for each distinct
   mount whose type is in `kinds` (tmpfs only for /dev/shm and /tmp)."
   def lines = st.split(read("/proc/self/mountinfo", 1048576), "\n")
   mut out = [], seen = {}
   mut int i = 0
   while i < lines.len {
      def line = to_str(lines.get(i, ""))
      def sep = st.find(line, " - ")
      if sep > 0 {
         def a = st.split_words(slice(line, 0, sep)), b = st.split_words(slice(line, sep + 3, line.len))
         def kind = to_str(b.get(0, "")), path = mount_unescape(to_str(a.get(4, "")))
         if kinds.contains(kind) && !seen.contains(path) && (kind != "tmpfs" || path == "/dev/shm" || path == "/tmp") {
            seen = seen.set(path, true)
            def cap = fs_capacity(path)
            out = out.append([path, kind, cap.get(0, -1), cap.get(1, -1)])
         }
      }
      i += 1
   }
   out
}

fn _sys_number(str path) f64 {
   def v = st.strip(read(path))
   if v.len == 0 { return -1.0 }
   float(_num(v))
}

fn gpus() list {
   "Returns [card, \"vendor:device\", busy_percent, vram_used, vram_total]
   for each DRM card ; unknown readings are -1."
   mut out = []
   def cards = sorted(fs.list_dir("/sys/class/drm"))
   mut int i = 0
   while i < cards.len {
      def name = to_str(cards.get(i, ""))
      if st.find(name, "card") == 0 && st.find(name, "-") < 0 {
         def base = "/sys/class/drm/" + name + "/device/"
         def vendor = st.strip(read(base + "vendor"))
         def device = st.strip(read(base + "device"))
         if vendor.len > 0 {
            out = out.append([name, vendor + ":" + device, _sys_number(base + "gpu_busy_percent"), _sys_number(base + "mem_info_vram_used"), _sys_number(base + "mem_info_vram_total")])
         }
      }
      i += 1
   }
   out
}

fn pressure() list {
   "Returns PSI rows [resource, \"some\"|\"full\", avg10, avg60, avg300] for
   cpu, memory and io from /proc/pressure."
   mut out = []
   def names = ["cpu", "memory", "io"]
   mut int i = 0
   while i < names.len {
      def k = to_str(names.get(i, ""))
      def lines = st.split(read("/proc/pressure/" + k), "\n")
      mut int j = 0
      while j < lines.len {
         def a = st.split_words(to_str(lines.get(j, "")))
         if a.len >= 5 {
            out = out.append([k, a.get(0), st.str_replace(to_str(a.get(1)), "avg10=", ""), st.str_replace(to_str(a.get(2)), "avg60=", ""), st.str_replace(to_str(a.get(3)), "avg300=", "")])
         }
         j += 1
      }
      i += 1
   }
   out
}

fn readlink(str path) str {
   "Returns the target of symbolic link `path` (e.g. /proc/PID/exe); \"\"
   when it is not a link or cannot be read."
   #if x86_64 || aarch64 {
      def ptr buf = bytes_new_raw(4097)
      if !buf { return "" }
      mut call = 267
      #aarch64 { call = 78 } #endif
      def n = sys.syscall(call, -100, path, buf, 4096)
      if n < 0 { return "" }
      return init_str(buf, n)
   } #else { return "" } #endif
}

fn pidfd_open(int pid) int {
   "Opens a pidfd for `pid` (Linux 5.3+); negative errno on failure."
   #if x86_64 || aarch64 { return sys.syscall(434, pid, 0) } #else { return -38 } #endif
}

fn pidfd_signal(int fd, int sig) int {
   "Sends `sig` through pidfd `fd`, which cannot hit a recycled PID;
   0 or a negative errno."
   #if x86_64 || aarch64 { return sys.syscall(424, fd, sig, 0, 0) } #else { return -38 } #endif
}
