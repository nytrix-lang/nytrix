#!/usr/bin/env ny

;; Keywords: ui gui os proc system monitor task-manager processes cpu memory disk network sensors
;; Task manager: live processes (sort, filter, details, pidfd-safe signals),
;; per-core CPU, memory and swap history, disk and network throughput,
;; hwmon sensors and batteries, PSI pressure, a disk-space scanner and a
;; socket viewer, read straight from /proc and /sys through core.os.proc.
;; Pages: F1 Processes, F2 Performance, F3 Disks, F4 Network, F6 Hardware,
;; F7 Pressure, F9 Space, F10 Tools, F11 Overview. F5 refresh, F8 pause sampling.
;; Env: NY_GTOP_PROBE=1 prints one headless sample; NY_GTOP_PAGE,
;; NY_GTOP_FRAMES and NY_GTOP_SNAPSHOT=PATH drive scripted runs.

use core
use core.common as common
use core.str as s
use core.math (min, max)
use core.os as os
use core.os.fs as fs
use core.os.info as info
use core.os.proc as proc
use core.format as fmt
use core.os.sys as sys
use core.os.ffi as ffi
use core.os.ui.render as gfx
use core.math.parse.img.svg as svg
use core.os.ui.render.viewer as viewer
use core.os.ui.render.viewer.bootstrap as bootstrap
use core.os.ui.render.viewer.gui as gui
use core.os.ui.window as window
use core.os.ui.window.consts

fn num(any x) int { s.atoi(to_str(x)) }
fn field(list a, int i) int { num(a.get(i, "0")) }
fn words(str x) list { s.split_words(x) }
fn delta(int now, int old) int { int(max(0, now - old)) }
fn val(dict d, str key, int fallback=-1) int {
   if !d.contains(key) { return fallback }
   num(words(to_str(d.get(key, "0"))).get(0, "0"))
}
fn history(list h, f64 v) list {
   mut out = h.len >= 120 ? slice(h, h.len - 119, h.len) : h
   out.append(v)
}
fn pair_rates(dict now, dict old, f64 dt, bool disk) list {
   mut out = []
   def names = sorted(now.keys())
   mut int i = 0
   while i < names.len {
      def name = to_str(names.get(i, ""))
      def a = now.get(name, []), b = old.get(name, [])
      mut r, w, busy = -1.0, -1.0, -1.0
      if b.len >= 2 && dt > 0.0 && int(a.get(0, 0)) >= int(b.get(0, 0)) && int(a.get(1, 0)) >= int(b.get(1, 0)) {
         r = float(int(a.get(0, 0)) - int(b.get(0, 0))) / dt
         w = float(int(a.get(1, 0)) - int(b.get(1, 0))) / dt
         if disk { busy = min(100.0, float(delta(a.get(2, 0), b.get(2, 0))) / (dt * 10.0)) }
      }
      out = out.append([name, r, w, busy])
      i += 1
   }
   out
}
fn sensor_value(str path, f64 divisor) f64 {
   def v = s.strip(proc.read(path,512))
   if v.len == 0 { return -1.0 }
   float(num(v)) / divisor
}

mut cpu_prev, net_prev, disk_prev, proc_prev = {}, {}, {}, {}
mut cpu_rows, net_rows, disk_rows, gpu_rows, sensor_rows, processes = [], [], [], [], [], []
mut mount_rows = []
mut disk_meta, net_meta, pressure_rows = {}, {}, []
mut last_metadata = 0
mut sleeping, stopped, zombies = 0, 0, 0
mut collect_peak = 0.0
mut memory_details = {}
mut cpu_hist, mem_hist, swap_hist, rx_hist, tx_hist, rd_hist, wr_hist = [], [], [], [], [], [], []
mut cpu_usage, memory_usage, swap_usage = -1.0, -1.0, -1.0
mut mem_total, mem_used, swap_total, swap_used = 0, 0, 0, 0
mut uptime = "", load = "", threads = 0, running = 0
mut last_sample = 0
mut sample_ms = 0.0
mut proc_errors = 0
mut user_names = proc.users()
def host = info.hostname()
def cpu_model = info.cpu_name()
mut logical = 1

mut dict gpu_metrics = {}, gpu_series = {}, device_series = {}, cpu_freq = {}, battery_rows = {}
mut list freq_hist = [], temp_hist = [], power_hist = []
mut f64 frequency = -1.0, temperature = -1.0, package_power = -1.0
mut dict energy_prev = {}
mut str gpu_choice = "", disk_choice = "", net_choice = ""
mut int watch_pid = 0, watch_start = -1
mut dict watch_hist = {}
fn metric(f64 v,str unit) str { v < 0.0 ? "N/A" : to_str(round(v,1))+" "+unit }
fn first_sensor(str base,list paths,f64 div) f64 {
   mut int i = 0
   while i < paths.len {
      def f64 v = sensor_value(base+to_str(paths.get(i,"")),div)
      if v >= 0.0 { return v }
      i += 1
   }
   -1.0
}
fn dpm_clock(str path) f64 {
   def lines = s.split(proc.read(path,4096),"\n")
   mut int i = 0
   while i < lines.len {
      def str line = to_str(lines.get(i,""))
      if s.find(line,"*") >= 0 { return float(num(s.str_replace(to_str(words(line).get(1,"0")),"Mhz",""))) }
      i += 1
   }
   -1.0
}
fn gpu_key(list g) str { to_str(g.get(0,""))+":"+to_str(g.get(1,"")) }
mut any nvml_lib = 0
mut bool nvml_attempted = false
mut dict nvml_symbols = {}
fn nvml_symbol(str n) any {
   if !nvml_lib { return 0 }
   if nvml_symbols.contains(n) { return nvml_symbols.get(n,0) }
   def f = ffi.dlsym(nvml_lib,n)
   nvml_symbols = own(nvml_symbols.set(n,f))
   f
}
fn nvml_start() bool {
   if nvml_attempted { return !(!nvml_lib) }
   nvml_attempted = true
   nvml_lib = ffi.dlopen("libnvidia-ml.so.1",2)
   if !nvml_lib { return false }
   def f = nvml_symbol("nvmlInit_v2")
   if !f || ffi.int32zero(f) != 0 { ffi.dlclose(nvml_lib) nvml_lib = 0 nvml_symbols = {} return false }
   true
}
fn nvml_stop() any {
   if !(!nvml_lib) {
      def f = nvml_symbol("nvmlShutdown")
      if f { ffi.int32zero(f) }
      ffi.dlclose(nvml_lib)
      nvml_lib = 0
      nvml_symbols = {}
   }
   0
}
fn pci_slot(str base) str {
   def lines = s.split(proc.read(base+"uevent",4096),"\n")
   mut int i = 0
   while i < lines.len {
      def str line = to_str(lines.get(i,""))
      if s.find(line,"PCI_SLOT_NAME=") == 0 { return to_str(slice(line,14,line.len)) }
      i += 1
   }
   ""
}
fn nvml_u32(any dev,ptr buf,str name,int arg=-1,f64 scale=1.0) f64 {
   def f = nvml_symbol(name)
   if !f { return -1.0 }
   def int rc = arg < 0 ? int(ffi.any2(f,dev,buf)) : int(ffi.any3(f,dev,arg,buf))
   rc == 0 ? float(load32(buf,0))/scale : -1.0
}
fn nvml_metrics(str slot,dict fallback) dict {
   if slot.len == 0 || !nvml_start() { return fallback }
   def handle = nvml_symbol("nvmlDeviceGetHandleByPciBusId_v2")
   if !handle { return fallback }
   def ptr buf = malloc(128)
   if !buf { return fallback }
   memset(buf,0,128)
   if int(ffi.any2(handle,ffi.cstr(slot),buf)) != 0 { free(buf) return fallback }
   def ptr dev = load64(buf,0)
   if !dev { free(buf) return fallback }
   mut dict out = fallback
   def name = nvml_symbol("nvmlDeviceGetName")
   if name && int(ffi.any3(name,dev,buf,128)) == 0 { store8(buf,0,127) out = out.set("name",s.cstr_to_str(buf)) }
   def util = nvml_symbol("nvmlDeviceGetUtilizationRates")
   if util && int(ffi.any2(util,dev,buf)) == 0 {
      out = out.set("load",float(load32(buf,0)))
      out = out.set("memload",float(load32(buf,4)))
   }
   def mem = nvml_symbol("nvmlDeviceGetMemoryInfo")
   if mem && int(ffi.any2(mem,dev,buf)) == 0 {
      def f64 total = float(load64_i(buf,0)), used = float(load64_i(buf,16))
      out = out.set("total",total).set("used",used).set("vram",total > 0.0 ? 100.0*used/total : -1.0)
   }
   def list keys = ["temp","clock","memclock","power","cap","fanpct"]
   def list calls = ["nvmlDeviceGetTemperature","nvmlDeviceGetClockInfo","nvmlDeviceGetClockInfo","nvmlDeviceGetPowerUsage","nvmlDeviceGetPowerManagementLimit","nvmlDeviceGetFanSpeed"]
   def list args = [0,0,2,-1,-1,-1]
   mut int i = 0
   while i < keys.len {
      def f64 v = nvml_u32(dev,buf,to_str(calls.get(i,"")),int(args.get(i,-1)),i == 3 || i == 4 ? 1000.0 : 1.0)
      if v >= 0.0 { out = out.set(to_str(keys.get(i,"")),v) }
      i += 1
   }
   free(buf)
   out
}
fn gpu_sample(str base,list g) dict {
   mut f64 clock = dpm_clock(base+"pp_dpm_sclk")
   if clock < 0.0 { clock = first_sensor(base,["gt_cur_freq_mhz","gt/gt0/rps_cur_freq_mhz","tile0/gt0/freq0/cur_freq"],1.0) }
   mut f64 temp = -1.0, hotspot = -1.0, memtemp = -1.0, power = -1.0, cap = -1.0, fan = -1.0, fanpct = -1.0
   def hw = sorted(fs.list_dir(base+"hwmon"))
   mut int i = 0
   while i < hw.len {
      def str h = base+"hwmon/"+to_str(hw.get(i,""))+"/"
      if temp < 0.0 { temp = sensor_value(h+"temp1_input",1000.0) }
      if hotspot < 0.0 && to_str(s.lower(s.strip(proc.read(h+"temp2_label",256)))) == "junction" { hotspot = sensor_value(h+"temp2_input",1000.0) }
      if memtemp < 0.0 && to_str(s.lower(s.strip(proc.read(h+"temp3_label",256)))) == "mem" { memtemp = sensor_value(h+"temp3_input",1000.0) }
      if power < 0.0 { power = first_sensor(h,["power1_average","power1_input"],1000000.0) }
      if cap < 0.0 { cap = sensor_value(h+"power1_cap",1000000.0) }
      if fan < 0.0 { fan = sensor_value(h+"fan1_input",1.0) }
      def f64 pwm = sensor_value(h+"pwm1",1.0), maxpwm = sensor_value(h+"pwm1_max",1.0)
      if fanpct < 0.0 && pwm >= 0.0 && maxpwm > 0.0 { fanpct = float(min(100.0,100.0*pwm/maxpwm)) }
      i += 1
   }
   def f64 used = float(g.get(3,-1.0)), total = float(g.get(4,-1.0))
   mut dict metrics = {"load":float(g.get(2,-1.0)),"used":used,"total":total,"vram":total > 0.0 && used >= 0.0 ? 100.0*used/total : -1.0,"clock":clock,"memclock":dpm_clock(base+"pp_dpm_mclk"),"temp":temp,"hotspot":hotspot,"memtemp":memtemp,"power":power,"cap":cap,"fan":fan,"fanpct":fanpct,"memload":sensor_value(base+"mem_busy_percent",1.0),"gtt":sensor_value(base+"mem_info_gtt_used",1.0)}
   metrics = metrics.set("name",s.strip(proc.read(base+"product_name",512)))
   if s.find(to_str(g.get(1,"")),"0x10de:") == 0 { return nvml_metrics(pci_slot(base),metrics) }
   metrics
}
fn collect_extra(f64 dt) any {
   mut dict gm = {}, gs = {}, ds = {}, cp = {}, ep = {}, batteries = {}
   mut int i = 0
   while i < gpu_rows.len {
      def list g = gpu_rows.get(i,[])
      def str k = gpu_key(g)
      def dict m = gpu_sample("/sys/class/drm/"+to_str(g.get(0,""))+"/device/",g)
      gm = gm.set(k,m)
      def dict prev = gpu_series.get(k,{})
      mut dict next = {}
      def keys = ["load","vram","clock","memclock","temp","power","fan","memload"]
      mut int j = 0
      while j < keys.len {
         def str n = keys.get(j,"")
         next = next.set(n,history(prev.get(n,[]),float(m.get(n,-1.0))))
         j += 1
      }
      gs = gs.set(k,next)
      i += 1
   }
   gpu_metrics = own(gm)
   gpu_series = own(gs)
   mut int kind = 0
   while kind < 2 {
      def list rows = kind == 0 ? disk_rows : net_rows
      i = 0
      while i < rows.len {
         def list r = rows.get(i,[])
         def str k = (kind == 0 ? "disk:" : "net:")+to_str(r.get(0,""))
         def list prev = device_series.get(k,[[],[],[]])
         ds = ds.set(k,[history(prev.get(0,[]),float(r.get(1,-1.0))),history(prev.get(1,[]),float(r.get(2,-1.0))),history(prev.get(2,[]),float(r.get(3,-1.0)))])
         i += 1
      }
      kind += 1
   }
   device_series = own(ds)
   mut f64 sum = 0.0, t = -1.0, watts = -1.0
   mut int count = 0
   i = 0
   while i < cpu_rows.len {
      def str k = to_str(cpu_rows.get(i,[]).get(0,""))
      def f64 v = first_sensor("/sys/devices/system/cpu/"+k+"/cpufreq/",["scaling_cur_freq","cpuinfo_cur_freq"],1000.0)
      if v >= 0.0 { sum += v count += 1 }
      cp = cp.set(k,v)
      i += 1
   }
   def hw = fs.list_dir("/sys/class/hwmon")
   i = 0
   while i < hw.len {
      def str b = "/sys/class/hwmon/"+to_str(hw.get(i,""))+"/"
      def str n = s.strip(proc.read(b+"name",256))
      if n == "k10temp" || n == "coretemp" || n == "zenpower" || n == "cpu_thermal" { t = float(max(t,sensor_value(b+"temp1_input",1000.0))) }
      i += 1
   }
   def domains = fs.list_dir("/sys/class/powercap")
   i = 0
   while i < domains.len {
      def str n = to_str(domains.get(i,"")), b = "/sys/class/powercap/"+n+"/"
      if s.find(s.strip(proc.read(b+"name",256)),"package-") == 0 {
         def f64 v = sensor_value(b+"energy_uj",1.0), prev = float(energy_prev.get(n,-1.0)), range = sensor_value(b+"max_energy_range_uj",1.0)
         mut f64 diff = v-prev
         if diff < 0.0 && range > 0.0 { diff += range }
         if v >= 0.0 && prev >= 0.0 && diff >= 0.0 && dt > 0.0 { watts = float(max(0.0,watts))+diff/(dt*1000000.0) }
         if v >= 0.0 { ep = ep.set(n,v) }
      }
      i += 1
   }
   cpu_freq = own(cp)
   energy_prev = own(ep)
   frequency = count > 0 ? sum/float(count) : -1.0
   temperature = t
   package_power = watts
   freq_hist = own(history(freq_hist,frequency))
   temp_hist = own(history(temp_hist,temperature))
   power_hist = own(history(power_hist,package_power))
   def supplies = fs.list_dir("/sys/class/power_supply")
   i = 0
   while i < supplies.len {
      def str n = to_str(supplies.get(i,"")), b = "/sys/class/power_supply/"+n+"/"
      if s.strip(proc.read(b+"type",256)) == "Battery" { batteries = batteries.set(n,[sensor_value(b+"capacity",1.0),s.strip(proc.read(b+"status",256)),sensor_value(b+"power_now",1000000.0)]) }
      i += 1
   }
   battery_rows = own(batteries)
   0
}

mut bool scan_active = false
mut int scan_index = 0, scan_started = 0, scan_work = 0, scan_elapsed = 0, scan_ticks = 0, proc_ticks = -1, proc_stamp = 0, scan_completed = 0
mut f64 scan_dt = 0.0, scan_wall_ms = 0.0
mut int scan_threads = 0, scan_running = 0, scan_sleeping = 0, scan_stopped = 0, scan_zombies = 0, scan_errors = 0
mut list scan_pids = [], scan_rows = []
mut dict scan_next = {}
fn stat_parent(str raw) int {
   def int pos = s.find_last(raw,")")
   if pos < 0 { return -1 }
   def list f = words(slice(raw,pos+1,raw.len))
   f.len >= 2 ? num(f.get(1,"-1")) : -1
}
fn collect_begin() any {
   if scan_active { return 0 }
   def started = os.ticks()
   def dt = last_sample > 0 ? float(started - last_sample) / 1000000000.0 : 0.0
   def cp = proc.cpu_times()
   def total = cp.get("cpu", [0, 0]), prev = cpu_prev.get("cpu", [0, 0])
   def int total_ticks = int(total.get(0,0))
   scan_elapsed = proc_ticks >= 0 ? delta(total_ticks,proc_ticks) : 0
   scan_ticks = total_ticks
   scan_dt = proc_stamp > 0 ? float(started-proc_stamp)/1000000000.0 : 0.0
   mut keys = []
   def names = cp.keys()
   mut ki = 0
   while ki < names.len {
      def k = to_str(names.get(ki, ""))
      if k != "cpu" { keys = keys.append(num(slice(k, 3, k.len))) }
      ki += 1
   }
   def ids = sorted(keys)
   keys = ["cpu"]
   ki = 0
   while ki < ids.len { keys = keys.append("cpu" + to_str(ids.get(ki, 0))) ki += 1 }
   mut cr = []
   mut int i = 0
   while i < keys.len {
      def k = to_str(keys.get(i, ""))
      def a = cp.get(k, [0, 0]), b = cpu_prev.get(k, [0, 0])
      def d = delta(a.get(0, 0), b.get(0, 0))
      def usage = cpu_prev.contains(k) && d > 0 ? min(100.0, max(0.0, 100.0 * float(d - delta(a.get(1, 0), b.get(1, 0))) / float(d))) : -1.0
      if k == "cpu" { cpu_usage = usage }
      else { cr = cr.append([k, usage]) }
      i += 1
   }
   cpu_rows = own(cr)
   if cr.len > 0 { logical = cr.len }
   cpu_prev = own(cp)
   def mem = proc.kv(proc.read("/proc/meminfo",8192))
   memory_details = own(mem)
   mem_total = int(max(0, val(mem, "MemTotal"))) * 1024
   def available = val(mem, "MemAvailable")
   if available >= 0 && mem_total > 0 {
      mem_used = int(max(0, mem_total - available * 1024))
      memory_usage = 100.0 * float(mem_used) / float(mem_total)
   } else { memory_usage = -1.0 }
   swap_total = int(max(0, val(mem, "SwapTotal"))) * 1024
   swap_used = int(max(0, swap_total - int(max(0, val(mem, "SwapFree"))) * 1024))
   swap_usage = swap_total > 0 ? 100.0 * float(swap_used) / float(swap_total) : 0.0
   def net = proc.net_counters(), disks = proc.disk_counters()
   net_rows = own(pair_rates(net, net_prev, dt, false))
   disk_rows = own(pair_rates(disks, disk_prev, dt, true))
   net_prev = own(net)
   disk_prev = own(disks)
   mut rx, tx, rd, wr = -1.0, -1.0, -1.0, -1.0
   i = 0
   while i < net_rows.len {
      def r = net_rows.get(i, [])
      if to_str(r.get(0, "")) != "lo" && float(r.get(1, -1.0)) >= 0.0 { rx = float(max(0.0, rx)) + float(r.get(1, 0.0)) tx = float(max(0.0, tx)) + float(r.get(2, 0.0)) }
      i += 1
   }
   i = 0
   while i < disk_rows.len {
      def r = disk_rows.get(i, [])
      if float(r.get(1, -1.0)) >= 0.0 { rd = float(max(0.0, rd)) + float(r.get(1, 0.0)) wr = float(max(0.0, wr)) + float(r.get(2, 0.0)) }
      i += 1
   }
   cpu_hist = own(history(cpu_hist, cpu_usage))
   mem_hist = own(history(mem_hist, memory_usage))
   swap_hist = own(history(swap_hist, swap_usage))
   rx_hist = own(history(rx_hist, rx))
   tx_hist = own(history(tx_hist, tx))
   rd_hist = own(history(rd_hist, rd))
   wr_hist = own(history(wr_hist, wr))
   gpu_rows = own(proc.gpus())
   collect_extra(dt)
   if last_metadata == 0 || started - last_metadata >= 5000000000 {
      mount_rows = own(proc.mounts())
      sensor_rows = own(proc.sensors())
      user_names = own(proc.users())
      last_metadata = started
   }
   mut dm, nm = {}, {}
   i = 0
   while i < disk_rows.len {
      def k = to_str(disk_rows.get(i, []).get(0, ""))
      def sectors = sensor_value("/sys/block/" + k + "/size", 1.0)
      dm = dm.set(k, sectors < 0.0 ? -1.0 : sectors * 512.0)
      i += 1
   }
   i = 0
   while i < net_rows.len {
      def k = to_str(net_rows.get(i, []).get(0, ""))
      nm = nm.set(k, s.strip(proc.read("/sys/class/net/" + k + "/operstate")))
      i += 1
   }
   disk_meta = own(dm)
   net_meta = own(nm)
   pressure_rows = own(proc.pressure())
   uptime = to_str(words(proc.read("/proc/uptime",256)).get(0, "0"))
   load = s.join(slice(words(proc.read("/proc/loadavg",256)), 0, 3), "  /  ")
   scan_pids = own(fs.list_dir("/proc"))
   scan_rows = list(scan_pids.len)
   scan_next = dict(int(max(16,scan_pids.len*2)))
   scan_index = 0
   scan_threads, scan_running, scan_sleeping, scan_stopped, scan_zombies, scan_errors = 0,0,0,0,0,0
   scan_started = started
   last_sample = started
   scan_work = os.ticks()-started
   scan_active = true
   0
}
fn collect_cancel() any {
   scan_active = false
   scan_pids = []
   scan_rows = []
   scan_next = {}
   0
}
fn collect_step(int limit=64,int budget=5) bool {
   if !scan_active { return false }
   def int began = os.ticks()
   mut int i = scan_index, done = 0
   mut list rows = scan_rows
   mut dict next = scan_next
   while i < scan_pids.len && done < limit {
      if done > 0 && os.ticks()-began >= budget*1000000 { break }
      done += 1
      def name = to_str(scan_pids.get(i, ""))
      def pid = num(name)
      if pid > 0 && to_str(pid) == name {
         def base = "/proc/" + name + "/"
         def str raw_stat = proc.read(base + "stat", 4096)
         def st = proc.pid_stat(raw_stat)
         if st.len == 10 {
            def old = proc_prev.get(pid, [])
            def same = old.len == 4 && int(old.get(0, -1)) == int(st.get(7, -2))
            def status = proc.read(base + "status", 16384)
            def rss = proc.kv_int(status, "VmRSS")
            def uid = proc.kv_int(status, "Uid")
            def io = proc.read(base + "io", 2048)
            def r = proc.kv_int(io, "read_bytes"), w = proc.kv_int(io, "write_bytes")
            def pcpu = same && scan_elapsed > 0 ? 100.0 * float(delta(st.get(4, 0), old.get(1, 0))) * float(logical) / float(scan_elapsed) : -1.0
            def pr = same && scan_dt > 0.0 && r >= 0 && int(old.get(2, -1)) >= 0 && r >= int(old.get(2, 0)) ? float(r - int(old.get(2, 0))) / scan_dt : -1.0
            def pw = same && scan_dt > 0.0 && w >= 0 && int(old.get(3, -1)) >= 0 && w >= int(old.get(3, 0)) ? float(w - int(old.get(3, 0))) / scan_dt : -1.0
            def check = proc.pid_stat(proc.read(base + "stat", 4096))
            if check.len != 10 || int(check.get(7, -1)) != int(st.get(7, -2)) { scan_errors += 1 i += 1 continue }
            next = next.set(pid, [st.get(7), st.get(4), r, w])
            rows = rows.append([pid, st.get(1), to_str(user_names.get(uid, uid < 0 ? "?" : to_str(uid))), pcpu, rss < 0 ? -1 : rss * 1024, pr, pw, st.get(2), field(st,6), field(st,5), stat_parent(raw_stat), int(st.get(7,-1)), uid, field(st,8), field(st,9), r, w])
            if pid == watch_pid && int(st.get(7,-1)) == watch_start { watch_hist = own({"cpu":history(watch_hist.get("cpu",[]),pcpu),"rss":history(watch_hist.get("rss",[]),rss < 0 ? -1.0 : float(rss)*1024.0),"read":history(watch_hist.get("read",[]),pr),"write":history(watch_hist.get("write",[]),pw)}) }
            scan_threads += int(st.get(6, 0))
            def str state = to_str(st.get(2, ""))
            if state == "R" { scan_running += 1 }
            elif state == "Z" { scan_zombies += 1 }
            elif state == "T" || state == "t" { scan_stopped += 1 }
            else { scan_sleeping += 1 }
         } else { scan_errors += 1 }
      }
      i += 1
   }
   scan_index = i
   scan_work += os.ticks()-began
   if i < scan_pids.len {
      scan_rows = own(rows)
      scan_next = own(next)
      return false
   }
   proc_prev = own(next)
   processes = own(rows)
   proc_stamp = scan_started
   proc_ticks = scan_ticks
   threads, running, sleeping, stopped, zombies, proc_errors = scan_threads,scan_running,scan_sleeping,scan_stopped,scan_zombies,scan_errors
   scan_completed = os.ticks()
   scan_wall_ms = float(scan_completed-scan_started)/1000000.0
   sample_ms = float(scan_work)/1000000.0
   collect_peak = max(collect_peak,sample_ms)
   collect_cancel()
   true
}
fn collect() any {
   collect_begin()
   while scan_active { collect_step(1024,1000) }
   0
}

mut query = "", sort_col = 3, descending = true, selected_pid = 0, selected_start = -1
mut visible = [], dirty = true, tree_mode = false, own_only = false
mut selected_command = "", action_status = "Ready"
mut int action_status_until = 0, tool_message_until = 0, space_copy_until = 0
mut int last_proc_click_pid = 0, last_proc_click_ticks = 0
mut int last_mount_click_ticks = 0, last_space_click_ticks = 0
mut str last_mount_click = "", last_space_click = "", detail_name = ""
mut int detail_tab = 0
mut bool secondary_pressed = false, inspector_open = false, menu_key = false
mut int tools_view = 0
mut detail_text = ""
mut int detail_loaded = -1
mut pending = []
mut f64 view_w = 1280.0, view_h = 820.0
mut bool paused = false
mut int interval = 1
mut int page = int(max(0, min(8, s.atoi(common.env_trim("NY_GTOP_PAGE")))))
mut selected_row = []
mut detail_pid, detail_start = 0, -1
mut int state_filter = 0
mut int process_page_index = 0
mut int previous_page_index = -1
mut int page_size_index = 0
mut int table_mode = 0
mut int detail_line_count = 0
mut detail_lines = []
def page_sizes = [50, 100, 250]
def state_labels = ["All states", "Running", "Sleeping", "Stopped", "Zombie"]
def table_labels = ["Overview", "Memory / I/O"]
fn state_matches(str state, int filter) bool {
   if filter == 0 { return true }
   if filter == 1 { return state == "R" }
   if filter == 3 { return state == "T" || state == "t" }
   if filter == 4 { return state == "Z" }
   state != "R" && state != "T" && state != "t" && state != "Z"
}
fn query_matches(list p, str q) bool {
   if q.len == 0 { return true }
   def hay = s.lower(to_str(p.get(1, "")) + " " + to_str(p.get(0, 0)) + " " + to_str(p.get(2, "")) + " " + to_str(p.get(10, 0)))
   def terms = words(q)
   mut int i = 0
   while i < terms.len {
      if s.find(hay, to_str(terms.get(i, ""))) < 0 { return false }
      i += 1
   }
   true
}

def own_pid = os.pid()
def own_uid = os.uid()
fn before(list a, list b, int col, bool desc) bool {
   if col == 1 || col == 2 || col == 7 {
      def str x = s.lower(to_str(a.get(col, "")))
      def str y = s.lower(to_str(b.get(col, "")))
      if x == y { return int(a.get(0, 0)) <= int(b.get(0, 0)) }
      return desc ? x > y : x < y
   }
   if col == 3 || col == 5 || col == 6 {
      def f64 x = float(a.get(col, -1.0)), y = float(b.get(col, -1.0))
      if x < 0.0 && y >= 0.0 { return false }
      if y < 0.0 && x >= 0.0 { return true }
      if x == y { return int(a.get(0, 0)) <= int(b.get(0, 0)) }
      return desc ? x > y : x < y
   }
   def int x = int(a.get(col, -1)), y = int(b.get(col, -1))
   if col == 4 || col >= 13 {
      if x < 0 && y >= 0 { return false }
      if y < 0 && x >= 0 { return true }
   }
   if x == y { return int(a.get(0, 0)) <= int(b.get(0, 0)) }
   desc ? x > y : x < y
}
fn merge_sort(list a, int col, bool desc) list {
   if a.len < 2 { return a }
   def half = int(a.len / 2)
   def l = merge_sort(slice(a, 0, half), col, desc)
   def r = merge_sort(slice(a, half, a.len), col, desc)
   mut out = list(a.len), i = 0, j = 0
   while i < l.len || j < r.len {
      if j >= r.len || (i < l.len && before(l.get(i, []), r.get(j, []), col, desc)) { out = out.append(l.get(i, [])) i += 1 }
      else { out = out.append(r.get(j, [])) j += 1 }
   }
   out
}
fn tree_add(list out, dict children, int pid, int depth) list {
   if depth >= 128 { return out }
   mut rows = out
   def kids = children.get(pid, [])
   mut int i = 0
   while i < kids.len {
      def row = kids.get(i, [])
      rows = rows.append([row, depth])
      rows = tree_add(rows, children, int(row.get(0, 0)), depth + 1)
      i += 1
   }
   rows
}
fn rebuild() any {
   mut filtered = list(processes.len), known = {}, children = {}, rows = list(processes.len)
   def q = s.lower(s.strip(query))
   mut int i = 0
   selected_row = []
   while i < processes.len {
      def p = processes.get(i, [])
      if (!own_only || int(p.get(12, -1)) == own_uid) && state_matches(to_str(p.get(7, "")), state_filter) && query_matches(p, q) {
         if int(p.get(0, 0)) == selected_pid && int(p.get(11, -1)) == selected_start { selected_row = p }
         filtered = filtered.append(p)
         if tree_mode { known = known.set(p.get(0), true) }
      }
      i += 1
   }
   def ordered = merge_sort(filtered, sort_col, descending)
   i = 0
   while i < ordered.len {
      def r = ordered.get(i, [])
      if tree_mode {
         def ppid = int(r.get(10, 0))
         def parent = known.contains(ppid) && ppid != int(r.get(0, 0)) ? ppid : 0
         children = children.set(parent, children.get(parent, []).append(r))
      } else { rows = rows.append([r, 0]) }
      i += 1
   }
   if tree_mode {
      rows = tree_add([], children, 0, 0)
      if rows.len < ordered.len {
         mut emitted = {}
         i = 0
         while i < rows.len { emitted = emitted.set(rows.get(i, []).get(0, []).get(0, 0), true) i += 1 }
         i = 0
         while i < ordered.len {
            def r = ordered.get(i, [])
            if !emitted.contains(r.get(0, 0)) { rows = rows.append([r, 0]) }
            i += 1
         }
      }
   }
   visible = own(rows)
   dirty = false
   if selected_row.len == 0 { selected_pid = 0 selected_start = -1 selected_command = "" }
   0
}
fn set_action_status(str msg) any { action_status = own(msg) action_status_until = os.ticks() + 4000000000 0 }
fn set_tool_message(str msg) any { tool_message = own(msg) tool_message_until = os.ticks() + 4000000000 0 }
fn set_space_copy_status(str msg) any { space_copy_status = own(msg) space_copy_until = os.ticks() + 4000000000 0 }
fn active_status(str msg, int until) str { until > 0 && os.ticks() < until ? msg : "" }
fn select_process(list p) any {
   selected_pid = int(p.get(0, 0))
   selected_start = int(p.get(11, -1))
   selected_row = p
   if watch_pid != selected_pid || watch_start != selected_start { watch_pid, watch_start = selected_pid, selected_start watch_hist = own({"cpu":[float(p.get(3,-1.0))],"rss":[float(p.get(4,-1.0))],"read":[float(p.get(5,-1.0))],"write":[float(p.get(6,-1.0))]}) }
   def base = "/proc/" + to_str(selected_pid) + "/"
   selected_command = own(s.str_replace(proc.read(base + "cmdline", 16384), chr(0), " "))
   if !identity_matches(selected_pid, selected_start) { selected_command = "Process exited or changed identity." }
   0
}
fn step_process_selection(int delta_idx) any {
   if visible.len == 0 { return 0 }
   mut int cur = -1
   mut int i = 0
   while i < visible.len {
      def p = visible.get(i, []).get(0, [])
      if int(p.get(0, 0)) == selected_pid && int(p.get(11, -1)) == selected_start { cur = i break }
      i += 1
   }
   mut int next = cur < 0 ? (delta_idx > 0 ? 0 : visible.len - 1) : int(max(0, min(visible.len - 1, cur + delta_idx)))
   def list row = visible.get(next, []).get(0, [])
   if row.len > 0 {
      select_process(row)
      def int per_page = int(page_sizes.get(page_size_index, 50))
      process_page_index = int(next / per_page)
   }
   0
}
fn identity_matches(int pid, int start) bool { proc.pid_stat_valid(pid, start) }
fn detail_refresh() any {
   def base = "/proc/" + to_str(detail_pid) + "/"
   if !identity_matches(detail_pid, detail_start) {
      detail_text = "Process exited or changed identity."
      detail_lines = [detail_text]
      detail_line_count = 1
      detail_loaded = detail_tab
      return 0
   }
   if detail_tab == 0 {
      detail_text = own("Executable: " + proc.readlink(base + "exe") + "\nWorking directory: " + proc.readlink(base + "cwd") + "\n\n" + proc.read(base + "status"))
   } elif detail_tab == 1 { detail_text = own(proc.read(base + "maps", 262144)) }
   elif detail_tab == 4 { detail_text = own(proc.read(base + "io")) }
   elif detail_tab == 5 { detail_text = own(proc.read(base + "cgroup")) }
   else {
      def names = sorted(fs.list_dir(base + (detail_tab == 2 ? "fd" : "task")))
      mut lines = []
      mut int i = 0
      while i < names.len && i < 1000 {
         def id = to_str(names.get(i, ""))
         if detail_tab == 2 { lines = lines.append(id + "  " + proc.readlink(base + "fd/" + id)) }
         else { lines = lines.append(id + "  " + s.strip(proc.read(base + "task/" + id + "/comm",256))) }
         i += 1
      }
      if i < names.len { lines = lines.append("... truncated") }
      detail_text = own(s.join(lines, "\n"))
   }
   if !identity_matches(detail_pid, detail_start) { detail_text = "Process exited or changed identity." }
   detail_lines = own(s.split(detail_text, "\n"))
   detail_line_count = detail_lines.len
   detail_loaded = detail_tab
   0
}
fn target_valid(int pid, int start) bool {
   if pid <= 1 || pid == own_pid || start < 0 { return false }
   identity_matches(pid, start)
}
fn send_signal(int pid, int start, int sig) str {
   if !target_valid(pid, start) { return "Process exited, changed identity or is protected." }
   def fd = proc.pidfd_open(pid)
   if fd < 0 { return "pidfd_open failed: " + to_str(fd) }
   defer { sys.sys_close_quiet(fd) }
   if !target_valid(pid, start) { return "Process identity changed." }
   def rc = proc.pidfd_signal(fd, sig)
   rc == 0 ? "Signal sent to PID " + to_str(pid) : "Signal failed: " + to_str(rc)
}
fn queue_signal(int sig, str label) any {
   if selected_row.len > 0 && selected_pid > 1 && selected_pid != own_pid {
      pending = own([selected_pid, selected_start, sig, label, selected_row.get(1, "?")])
      gui.open_popup("confirm")
   }
   0
}
fn action_popup() any {
   if gui.begin_popup("confirm", float(min(420.0 * gui.scale(), view_w - 24.0 * gui.scale()))) {
      if pending.len == 5 {
         gui.text(to_str(pending.get(3)) + " " + to_str(pending.get(4)) + "?")
         gui.text_colored("PID " + to_str(pending.get(0)) + " - verify the target", "text_dim")
         if int(pending.get(2)) == 9 { gui.text_colored("Force kill may lose unsaved work.", "warn") }
         gui.layout_row([0.0, 0.0])
         if gui.button("cancel", "Cancel") { gui.close_popup() pending = [] }
         if gui.button("confirm_action", "Confirm") && pending.len == 5 {
            set_action_status(send_signal(int(pending.get(0)), int(pending.get(1)), int(pending.get(2))))
            pending = []
            gui.close_popup()
         }
      }
      gui.end_popup()
   }
   0
}
mut int font_body = 0, font_title = 0, font_small = 0
mut list blue = [0.18, 0.76, 0.88, 1.0]
mut list green = [0.26, 0.76, 0.59, 1.0]
mut list purple = [0.67, 0.56, 0.96, 1.0]
mut list orange = [0.97, 0.66, 0.35, 1.0]
def page_names = ["Processes", "Performance", "Disks", "Network", "Hardware", "Pressure", "Space", "Tools", "Overview"]
def page_subtitles = ["Inspect, filter and manage running tasks", "CPU, memory and resource history", "Device throughput and mounted filesystems", "Interface traffic and link status", "Graphics, temperatures, fans and power", "Kernel stalls and memory accounting", "Read-only disk space analysis", "System inventory and connections", "CPU, GPU, memory, disks, network and top processes"]
fn narrow(f64 width=760.0) bool { gui.remaining_w() < width * gui.scale() }
fn usage(f64 v) str { v < 0.0 ? "N/A" : fmt.fmt_pct(v) }
fn palette(str mode) any {
   gui.set_theme(mode)
   if mode == "dark" {
      blue = [0.18, 0.76, 0.88, 1.0]
      green = [0.26, 0.76, 0.59, 1.0]
      purple = [0.67, 0.56, 0.96, 1.0]
      orange = [0.97, 0.66, 0.35, 1.0]
      gui.set_theme_color("window", [0.0, 0.0, 0.0, 1.0])
      gui.set_theme_color("header", [0.070, 0.075, 0.080, 1.0])
      gui.set_theme_color("surface", [0.040, 0.043, 0.047, 1.0])
      gui.set_theme_color("surface_hover", [0.095, 0.13, 0.15, 1.0])
      gui.set_theme_color("surface_active", [0.09, 0.20, 0.23, 1.0])
      gui.set_theme_color("popup", [0.07, 0.08, 0.095, 1.0])
      gui.set_theme_color("border", [0.14, 0.16, 0.18, 1.0])
      gui.set_theme_color("text", [0.91, 0.94, 0.98, 1.0])
      gui.set_theme_color("text_dim", [0.62, 0.67, 0.73, 1.0])
      gui.set_theme_color("text_faint", [0.45, 0.52, 0.62, 1.0])
      gui.set_theme_color("accent", blue)
      gui.set_theme_color("ok", green)
      gui.set_theme_color("warn", orange)
      gui.set_theme_color("danger", [0.98, 0.34, 0.38, 1.0])
   } else {
      blue = [0.05, 0.42, 0.68, 1.0]
      green = [0.08, 0.48, 0.32, 1.0]
      purple = [0.38, 0.24, 0.72, 1.0]
      orange = [0.75, 0.38, 0.04, 1.0]
      gui.set_theme_color("window", [1.0, 1.0, 1.0, 1.0])
      gui.set_theme_color("header", [0.94, 0.95, 0.97, 1.0])
      gui.set_theme_color("surface", [0.97, 0.98, 0.99, 1.0])
      gui.set_theme_color("surface_hover", [0.89, 0.92, 0.96, 1.0])
      gui.set_theme_color("surface_active", [0.82, 0.88, 0.95, 1.0])
      gui.set_theme_color("popup", [1.0, 1.0, 1.0, 1.0])
      gui.set_theme_color("border", [0.78, 0.81, 0.86, 1.0])
      gui.set_theme_color("text", [0.05, 0.06, 0.08, 1.0])
      gui.set_theme_color("text_dim", [0.28, 0.31, 0.37, 1.0])
      gui.set_theme_color("text_faint", [0.48, 0.52, 0.59, 1.0])
      gui.set_theme_color("accent", blue)
      gui.set_theme_color("ok", green)
      gui.set_theme_color("warn", orange)
      gui.set_theme_color("danger", [0.78, 0.14, 0.14, 1.0])
   }
   0
}
fn measure_small(str t) f64 { float(gfx.measure_text_fast(font_small,t).get(0,0.0)) }
fn axis_value(f64 v,f64 ceiling,str unit) str {
   if unit == "bytes" { return fmt.fmt_bytes(v) }
   if unit != "" { return metric(v,unit) }
   ceiling > 0.0 ? fmt.fmt_pct(v) : fmt.fmt_rate(v)
}
fn graph(str id, str title, list values, f64 ceiling, list color, f64 h=104.0,str unit="") any {
   mut f64 top = ceiling
   if top <= 0.0 {
      top = 1.0
      mut int j = 0
      while j < values.len { top = float(max(top, float(values.get(j, 0.0)))) j += 1 }
   }
   def f64 limit = float(top)
   def f64 z = gui.scale()
   def rect = gui.grid_canvas(id, "", 0.0, h * z, 24.0, 4)
   def f64 rx = float(rect.get(0, 0.0))
   def f64 ry = float(rect.get(1, 0.0))
   def f64 rw = float(rect.get(2, 0.0))
   def f64 rh = float(rect.get(3, 0.0))
   def f64 title_h = float(gfx.measure_text_fast(font_body, "Ag0y").get(1, 16.0))
   def f64 small_h = float(gfx.measure_text_fast(font_small, "Ag0y").get(1, 14.0))
   def f64 hdr_h = float(max(26.0 * z, title_h + 10.0 * z))
   def f64 ftr_h = float(max(22.0 * z, small_h + 8.0 * z))
   def f64 labelw = float(max(measure_small(axis_value(limit,ceiling,unit)),max(measure_small(axis_value(limit*0.5,ceiling,unit)),measure_small(axis_value(0.0,ceiling,unit)))))
   def int px0 = int(rx + 10.0 * z + 0.5)
   def int py0 = int(ry + hdr_h + 4.0 * z + 0.5)
   def int pw = int(max(4.0, rw - labelw - 28.0 * z + 0.5))
   def int ph = int(max(4.0, rh - hdr_h - ftr_h - 6.0 * z + 0.5))
   def f64 x = float(px0), y = float(py0), w = float(pw), hh = float(ph)
   gui.push_clip_rect(rect)
   gui.paint_rect(rx + 1.0, ry + 1.0, rw - 2.0, rh - 2.0, gui.theme_color("surface"))
   gui.paint_rect(rx + 1.0, ry + 1.0, rw - 2.0, hdr_h, gui.theme_color("header"))
   gui.paint_rect(rx + 1.0, ry + 1.0, rw - 2.0, 2.5 * z, color)
   gui.paint_rect(x, y, w, hh, gui.theme_color("window"))
   mut int vline = 1
   while vline < 12 {
      def int gx = px0 + int(float(pw - 1) * float(vline) / 12.0 + 0.5)
      gui.paint_rect(float(gx), y, 1.0, hh, gui.theme_color_alpha("border", 0.35))
      vline += 1
   }
   gui.push_clip_rect([x, ry + 3.5 * z, max(1.0, rw - 20.0 * z), max(1.0, hdr_h - 3.5 * z)])
   gui.paint_text(x, ry + 3.5 * z + float(max(0.0, (hdr_h - 3.5 * z - title_h) * 0.5)), title, color)
   gui.pop_clip_rect()
   mut int k = 0
   while k <= 4 {
      def int gy = py0 + int(float(ph - 1) * float(k) / 4.0 + 0.5)
      gui.paint_rect(x, float(gy), w, 1.0, gui.theme_color_alpha("border", (k == 0 || k == 4) ? 0.90 : 0.65))
      if k % 2 == 0 {
         def f64 v = limit * (1.0 - float(k) / 4.0)
         def str label = axis_value(v,ceiling,unit)
         def f64 ly = k == 0 ? y : (k == 4 ? y + hh - small_h : float(gy) - small_h * 0.5)
         gui.paint_text(rx + rw - 10.0 * z - measure_small(label), ly, label, gui.theme_color("text_dim"), true)
      }
      k += 1
   }
   gui.paint_rect(x, y, 1.0, hh, gui.theme_color("border"))
   gui.paint_rect(float(px0 + pw - 1), y, 1.0, hh, gui.theme_color("border"))
   def int plot_x = px0 + 1, plot_y = py0 + 1, plot_w = int(max(1, pw - 2)), plot_h = int(max(1, ph - 2))
   def list fill_col = [color.get(0), color.get(1), color.get(2), 0.18]
   def f64 stroke_h = max(1.5, 1.8 * z)
   gui.push_clip_rect([float(plot_x), float(plot_y), float(plot_w), float(plot_h)])
   mut int prev_x = -1
   mut f64 prev_h = -1.0
   mut int i = 0
   while i < values.len {
      def f64 v = float(values.get(i, -1.0))
      def int sx0 = plot_x + int(float(120 - values.len + i) * float(plot_w) / 120.0 + 0.5)
      def int sx1 = plot_x + int(float(120 - values.len + i + 1) * float(plot_w) / 120.0 + 0.5)
      if v >= 0.0 {
         def f64 bh = float(min(float(plot_h), max(0.0, (v / limit) * float(plot_h))))
         if prev_x >= 0 && prev_h >= 0.0 && sx0 > prev_x {
            def int span = sx0 - prev_x
            if abs(bh - prev_h) < 0.75 {
               def f64 sy = float(min(float(plot_y + plot_h) - stroke_h, float(plot_y + plot_h) - bh))
               if bh > 0.0 { gui.paint_rect(float(prev_x), float(plot_y + plot_h) - bh, float(span), bh, fill_col) }
               gui.paint_rect(float(prev_x), sy, float(span), stroke_h, color)
            } else {
               mut int cx = prev_x
               while cx < sx0 {
                  def f64 t0 = float(cx - prev_x) / float(span)
                  def f64 t1 = float(cx + 1 - prev_x) / float(span)
                  def f64 ch0 = prev_h + (bh - prev_h) * t0
                  def f64 ch1 = prev_h + (bh - prev_h) * t1
                  def f64 ch = (ch0 + ch1) * 0.5
                  def f64 top_h = float(max(ch0, ch1))
                  def f64 seg_h = float(max(stroke_h, abs(ch1 - ch0) + stroke_h))
                  def f64 sy = float(min(float(plot_y + plot_h) - stroke_h, float(plot_y + plot_h) - top_h))
                  if ch > 0.0 { gui.paint_rect(float(cx), float(plot_y + plot_h) - ch, 1.0, ch, fill_col) }
                  gui.paint_rect(float(cx), sy, 1.0, seg_h, color)
                  cx += 1
               }
            }
         } else {
            def int bw = int(max(1, sx1 - sx0))
            def f64 sy = float(min(float(plot_y + plot_h) - stroke_h, float(plot_y + plot_h) - bh))
            if bh > 0.0 { gui.paint_rect(float(sx0), float(plot_y + plot_h) - bh, float(bw), bh, fill_col) }
            gui.paint_rect(float(sx0), sy, float(bw), stroke_h, color)
         }
         if i == values.len - 1 && sx1 > sx0 {
            def int bw = sx1 - sx0
            def f64 sy = float(min(float(plot_y + plot_h) - stroke_h, float(plot_y + plot_h) - bh))
            if bh > 0.0 { gui.paint_rect(float(sx0), float(plot_y + plot_h) - bh, float(bw), bh, fill_col) }
            gui.paint_rect(float(sx0), sy, float(bw), stroke_h, color)
         }
         prev_x = sx0
         prev_h = bh
      } else {
         prev_x = -1
         prev_h = -1.0
      }
      i += 1
   }
   gui.pop_clip_rect()
   def f64 fy = y + hh + float(max(2.0 * z, (ftr_h - small_h) * 0.5 - 1.0 * z))
   gui.paint_text(x, fy, to_str(values.len) + " / 120 samples", gui.theme_color("text_dim"), true)
   gui.paint_text(x + w - measure_small("now"), fy, "now", gui.theme_color("text_dim"), true)
   if gui.item_hovered() && values.len > 0 {
      def mouse = gui.mouse_xy()
      def f64 mx = float(mouse.get(0, 0.0))
      if mx >= float(plot_x) && mx < float(plot_x + plot_w) {
         def int ix = int((mx - float(plot_x)) * 120.0 / float(plot_w)) - 120 + values.len
         if ix >= 0 && ix < values.len {
            def f64 v = float(values.get(ix, -1.0))
            gui.paint_rect(mx, float(plot_y), 1.0, float(plot_h), gui.theme_color("text_dim"))
            gui.tooltip(v < 0.0 ? "Unavailable" : axis_value(v,ceiling,unit))
         }
      }
   }
   gui.pop_clip_rect()
   0
}
def icon_names = ["database","play","timer","history","gear","back","forward","close","nytrix","folder","home","search","copy","pause","refresh","sun","moon"]
def int ICON_RASTER = 192
mut dict ui_icons = {}
fn load_icons() int {
   mut int i = 0
   while i < icon_names.len {
      def str name = icon_names.get(i, "")
      def str path = "etc/assets/images/icons/" + name + ".svg"
      def im = svg.load_path(path, ICON_RASTER)
      if is_dict(im) {
         def int tex = gfx.texture_upload_image_ex(im, path, 37, true, false, 1, 33071, 33071, "gtop-icon:" + name, true)
         if tex > 0 { ui_icons = ui_icons.set(name, tex) }
      }
      i += 1
   }
   ui_icons.len
}
fn icon_ref(str name) int { int(ui_icons.get(name, -1)) }
fn free_icons() int {
   mut int i = 0
   while i < icon_names.len {
      def int tex = icon_ref(icon_names.get(i, ""))
      if tex > 0 { gfx.texture_destroy(tex) }
      i += 1
   }
   ui_icons = {}
   0
}
fn aligned_label(str label) any {
   gui.text("")
   def list rect = gui.last_item_rect()
   def f64 x = float(rect.get(0,0.0)), y = float(rect.get(1,0.0)), w = float(rect.get(2,0.0)), h = 26.0*gui.scale()
   def f64 fh = float(gfx.measure_text_fast(font_body,"Ag0y").get(1,16.0))
   gui.push_clip_rect([x,y,w,h])
   gui.paint_text(x+2.0*gui.scale(),y+float(max(0.0,(h-fh)*0.5)),label,gui.theme_color("text"))
   gui.pop_clip_rect()
   0
}
fn ibtn(str id, str icon, str label, bool active=false, f64 h=28.0) bool {
   def int tex = icon_ref(icon)
   if tex <= 0 { return gui.button(id, label.len > 0 ? label : icon, 0.0, h * gui.scale()) }
   gui.icon_button(id, tex, label, 0.0, h * gui.scale(), active)
}
fn menu_anchor(str id) any {
   def list r = gui.last_item_rect()
   gui.open_popup(id,float(r.get(0,0.0)),float(r.get(1,0.0))+float(r.get(3,0.0)))
}
fn resource_menu(int selected) int {
   mut int next = selected
   if gui.menu_item("cpu","CPU / memory","F2",selected == 1) { next = 1 }
   if gui.menu_item("gpu","GPU / sensors","F6",selected == 4) { next = 4 }
   if gui.menu_item("storage","Storage","F3",selected == 2) { next = 2 }
   if gui.menu_item("network","Network","F4",selected == 3) { next = 3 }
   if gui.menu_item("pressure","Pressure","F7",selected == 5) { next = 5 }
   next
}
fn utility_menu(int selected) int {
   mut int next = selected
   if gui.menu_item("space","Disk space","F9",selected == 6) { next = 6 }
   if gui.menu_item("system","System information","F10",selected == 7 && tools_view == 0) { next = 7 tools_view = 0 }
   if gui.menu_item("connections","Connections","",selected == 7 && tools_view == 1) { next = 7 tools_view = 1 }
   next
}
fn navigation(int selected,bool compact) int {
   mut int next = selected
   def f64 z = gui.scale()
   if compact {
      if gui.button("navigation",(selected == 7 ? (tools_view == 0 ? "System" : "Connections") : page_names.get(selected,"Overview"))+" / Menu",0.0,26.0*z) { menu_anchor("navigation_popup") }
   } else {
      if ibtn("overview_nav","home","Overview",selected == 8,26.0) { next = 8 }
      gui.tooltip("System overview (F11)")
      if ibtn("process_nav","history","Processes",selected == 0,26.0) { next = 0 }
      gui.tooltip("Process list (F1)")
      def bool resource = selected >= 1 && selected <= 5
      def names = ["Processes","CPU / RAM","Storage","Network","GPU","Pressure"]
      if ibtn("resources_nav","database",resource ? names.get(selected,"Resources") : "Resources",resource,26.0) { menu_anchor("resource_popup") }
      gui.tooltip("CPU (F2), Storage (F3), Network (F4), GPU (F6), Pressure (F7)")
      if ibtn("tools_nav","gear",selected == 6 ? "Space" : (selected == 7 ? (tools_view == 0 ? "System" : "Sockets") : "Tools"),selected == 6 || selected == 7,26.0) { menu_anchor("tools_popup") }
      gui.tooltip("Disk space (F9), System info (F10), Connections")
   }
   next
}
fn navigation_popups(int selected,bool compact) int {
   mut int next = selected
   def f64 z = gui.scale()
   if compact {
      if gui.begin_popup("navigation_popup",float(min(220.0*z,view_w-12.0*z))) {
         if gui.menu_item("overview","Overview","F11",selected == 8) { next = 8 }
         if gui.menu_item("processes","Processes","F1",selected == 0) { next = 0 }
         if gui.begin_menu("resources","Resources") { next = resource_menu(next) gui.end_menu() }
         if gui.begin_menu("utilities","Tools") { next = utility_menu(next) gui.end_menu() }
         gui.end_popup()
      }
   } else {
      if gui.begin_popup("resource_popup",220.0*z) { next = resource_menu(next) gui.end_popup() }
      if gui.begin_popup("tools_popup",220.0*z) { next = utility_menu(next) gui.end_popup() }
   }
   next
}
fn settings_menu() any {
   if gui.begin_popup("settings_popup",float(min(210.0*gui.scale(),view_w-12.0*gui.scale()))) {
      if gui.menu_item("theme","Light theme","",gui.theme() == "light") { palette(gui.theme() == "dark" ? "light" : "dark") }
      gui.menu_separator()
      def labels = ["0.5 seconds","1 second","2 seconds","5 seconds"]
      mut int i = 0
      while i < labels.len {
         if gui.menu_item("interval_"+to_str(i),labels.get(i,""),"",interval == i) { interval = i }
         i += 1
      }
      gui.end_popup()
   }
   0
}
fn filter_changed() any {
   dirty = true
   process_page_index = 0
   previous_page_index = -1
   0
}
fn process_options() any {
   if gui.begin_popup("process_options",float(min(225.0*gui.scale(),view_w-12.0*gui.scale()))) {
      if gui.menu_item("tree","Process tree","",tree_mode) { tree_mode = !tree_mode filter_changed() }
      if gui.menu_item("mine","My processes","",own_only) { own_only = !own_only filter_changed() }
      if gui.begin_menu("states","Process state") {
         mut int i = 0
         while i < state_labels.len {
            if gui.menu_item("state_"+to_str(i),state_labels.get(i,""),"",state_filter == i) { state_filter = i filter_changed() }
            i += 1
         }
         gui.end_menu()
      }
      if gui.begin_menu("columns","Columns") {
         mut int i = 0
         while i < table_labels.len {
            if gui.menu_item("mode_"+to_str(i),table_labels.get(i,""),"",table_mode == i) { table_mode = i process_page_index = 0 previous_page_index = -1 }
            i += 1
         }
         gui.end_menu()
      }
      if gui.begin_menu("rows","Rows per page") {
         mut int i = 0
         while i < page_sizes.len {
            if gui.menu_item("size_"+to_str(i),to_str(page_sizes.get(i,50)),"",page_size_index == i) { page_size_index = i process_page_index = 0 previous_page_index = -1 }
            i += 1
         }
         gui.end_menu()
      }
      gui.menu_separator()
      if gui.menu_item("reset","Clear filters","",false,query.len > 0 || tree_mode || own_only || state_filter != 0) { query = "" tree_mode = false own_only = false state_filter = 0 filter_changed() }
      gui.end_popup()
   }
   0
}
fn context_hit() bool { secondary_pressed && !inspector_open && !gui.any_popup_open() && gui.item_hovered() }
fn inspect_selected() any {
   if selected_pid <= 0 || selected_row.len == 0 { return 0 }
   detail_pid = selected_pid
   detail_start = selected_start
   detail_name = own(to_str(selected_row.get(1, "")))
   detail_loaded = -1
   detail_tab = 0
   inspector_open = true
   gui.show_window("inspector",true)
   gui.focus_window("inspector")
   0
}
fn process_actions() any {
   mut int signal = 0
   mut str label = ""
   mut bool details = false
   if gui.begin_popup("process_actions",float(min(235.0*gui.scale(),view_w-16.0*gui.scale()))) {
      def bool valid = selected_pid > 0 && selected_row.len > 0
      def bool allowed = valid && selected_pid > 1 && selected_pid != own_pid && selected_start >= 0
      gui.menu_item("target",valid ? "PID "+to_str(selected_pid) : "No process","",false,false)
      gui.menu_separator()
      if gui.menu_item("details","Process details","",false,valid) { details = true }
      if gui.menu_item("copy","Copy command","",false,valid && selected_command.len > 0) { set_action_status(os.set_clipboard_text(selected_command) ? "Command copied" : "Clipboard unavailable") }
      gui.menu_separator()
      if gui.menu_item("term","End task","",false,allowed) { signal = 15 label = "Terminate" }
      if gui.menu_item("stop","Suspend","",false,allowed) { signal = 19 label = "Suspend" }
      if gui.menu_item("resume","Resume","",false,allowed) { signal = 18 label = "Resume" }
      if gui.menu_item("kill","Force kill","",false,allowed) { signal = 9 label = "Force kill" }
      gui.end_popup()
   }
   if details { inspect_selected() }
   if signal != 0 { queue_signal(signal,label) }
   0
}

fn card_meter(list rect, f64 value, list color, list samples) any {
   if value < 0.0 { return 0 }
   def f64 z = gui.scale()
   def f64 x = float(rect.get(0, 0.0)) + 10.0 * z
   def f64 y = float(rect.get(1, 0.0))
   def f64 w = float(max(0.0, float(rect.get(2, 0.0)) - 20.0 * z))
   def f64 h = float(rect.get(3, 0.0))
   gui.paint_rect(x, y + h - 8.0*z, w, 3.0*z, gui.theme_color("border"))
   gui.paint_rect(x, y + h - 8.0*z, w * float(min(100.0, value)) / 100.0, 3.0*z, color)
   if w < 295.0*z || samples.len < 2 { return 0 }
   def int gx0 = int(x + w - w*0.36 + 0.5), gy0 = int(y + 29.0*z + 0.5)
   def int gw_i = int(max(4.0, w*0.36 + 0.5)), gh_i = int(max(4.0, 22.0*z + 0.5))
   def int n_start = int(max(0, samples.len-40))
   def list fill_col = [color.get(0),color.get(1),color.get(2),0.14]
   def f64 stroke_h = max(1.5, 1.5*z)
   mut int i = n_start
   mut int prev_x = -1
   mut f64 prev_h = -1.0
   while i < samples.len {
      def f64 v = float(samples.get(i,-1.0))
      def int sx0 = gx0 + int(float(40 - samples.len + i) * float(gw_i) / 40.0 + 0.5)
      def int sx1 = gx0 + int(float(40 - samples.len + i + 1) * float(gw_i) / 40.0 + 0.5)
      if v >= 0.0 {
         def f64 bh = float(min(float(gh_i), max(0.0, v*float(gh_i)/100.0)))
         if prev_x >= 0 && prev_h >= 0.0 && sx0 > prev_x {
            def int span = sx0 - prev_x
            if abs(bh - prev_h) < 0.75 {
               def f64 sy = float(min(float(gy0 + gh_i) - stroke_h, float(gy0 + gh_i) - bh))
               if bh > 0.0 { gui.paint_rect(float(prev_x), float(gy0 + gh_i) - bh, float(span), bh, fill_col) }
               gui.paint_rect(float(prev_x), sy, float(span), stroke_h, color)
            } else {
               mut int cx = prev_x
               while cx < sx0 {
                  def f64 t0 = float(cx - prev_x) / float(span)
                  def f64 t1 = float(cx + 1 - prev_x) / float(span)
                  def f64 ch0 = prev_h + (bh - prev_h) * t0
                  def f64 ch1 = prev_h + (bh - prev_h) * t1
                  def f64 ch = (ch0 + ch1) * 0.5
                  def f64 top_h = float(max(ch0, ch1))
                  def f64 seg_h = float(max(stroke_h, abs(ch1 - ch0) + stroke_h))
                  def f64 sy = float(min(float(gy0 + gh_i) - stroke_h, float(gy0 + gh_i) - top_h))
                  if ch > 0.0 { gui.paint_rect(float(cx), float(gy0 + gh_i) - ch, 1.0, ch, fill_col) }
                  gui.paint_rect(float(cx), sy, 1.0, seg_h, color)
                  cx += 1
               }
            }
         } else {
            def int bw = int(max(1, sx1 - sx0))
            def f64 sy = float(min(float(gy0 + gh_i) - stroke_h, float(gy0 + gh_i) - bh))
            if bh > 0.0 { gui.paint_rect(float(sx0), float(gy0 + gh_i) - bh, float(bw), bh, fill_col) }
            gui.paint_rect(float(sx0), sy, float(bw), stroke_h, color)
         }
         if i == samples.len - 1 && sx1 > sx0 {
            def int bw = sx1 - sx0
            def f64 sy = float(min(float(gy0 + gh_i) - stroke_h, float(gy0 + gh_i) - bh))
            if bh > 0.0 { gui.paint_rect(float(sx0), float(gy0 + gh_i) - bh, float(bw), bh, fill_col) }
            gui.paint_rect(float(sx0), sy, float(bw), stroke_h, color)
         }
         prev_x = sx0
         prev_h = bh
      } else { prev_x = -1 prev_h = -1.0 }
      i += 1
   }
   0
}
fn cards() any {
   def f64 z = gui.scale()
   def bool two = narrow(1100.0)
   gui.layout_row(two ? [0.0,0.0] : [0.0,0.0,0.0,0.0], 8.0*z)
   card_meter(gui.stat_card("cpu", "CPU", usage(cpu_usage), to_str(logical) + " logical CPUs", 0.0, 96.0*z, blue), cpu_usage, blue, cpu_hist)
   card_meter(gui.stat_card("memory", "MEMORY", usage(memory_usage), fmt.fmt_bytes(float(mem_used)) + " / " + fmt.fmt_bytes(float(mem_total)), 0.0, 96.0*z, purple), memory_usage, purple, mem_hist)
   if two { gui.layout_row([0.0,0.0],8.0*z) }
   gui.stat_card("tasks", "PROCESSES", to_str(processes.len), to_str(threads) + " threads", 0.0, 96.0*z, green)
   def averages = words(load)
   gui.stat_card("load", "LOAD AVERAGE / 1m", to_str(averages.get(0,"-")), "5m " + to_str(averages.get(2,"-")) + " / 15m " + to_str(averages.get(4,"-")), 0.0, 96.0*z, orange)
   0
}
fn pid_label(any v) str { is_int(v) && int(v) >= 0 ? to_str(int(v)) : "N/A" }
fn process_page() any {
   def f64 z = gui.scale()
   def bool tiny = narrow(760.0)
   if dirty { rebuild() }
   mut int per_page = int(page_sizes.get(page_size_index,50))
   mut int pages = int(max(1,(visible.len+per_page-1)/per_page))
   process_page_index = int(min(process_page_index,pages-1))
   gui.layout_row(tiny ? [0.0,30.0*z,30.0*z,54.0*z,30.0*z,30.0*z] : [0.0,105.0*z,100.0*z,75.0*z,32.0*z,32.0*z],4.0*z)
   def str q = gui.input_text("search","",query,"Search process, PID or user")
   if q != query { query = own(q) filter_changed() }
   def bool filtered = tree_mode || own_only || state_filter != 0 || query.len > 0
   if ibtn("options","gear",tiny ? "" : "Options",filtered || table_mode != 0,26.0) { menu_anchor("process_options") }
   gui.tooltip("Filters, columns and page size")
   gui.begin_disabled(selected_pid <= 0)
   if ibtn("actions","play",tiny ? "" : "Actions",false,26.0) { menu_anchor("process_actions") }
   gui.end_disabled()
   gui.tooltip("Process actions (Enter to inspect, Del to terminate)")
   aligned_label(to_str(process_page_index+1)+" / "+to_str(pages))
   gui.begin_disabled(process_page_index <= 0)
   if ibtn("prev_page","back","",false,26.0) { process_page_index -= 1 }
   gui.end_disabled()
   gui.begin_disabled(process_page_index+1 >= pages)
   if ibtn("next_page","forward","",false,26.0) { process_page_index += 1 }
   gui.end_disabled()
   process_options()
   if dirty { rebuild() }
   per_page = int(page_sizes.get(page_size_index,50))
   pages = int(max(1,(visible.len+per_page-1)/per_page))
   process_page_index = int(min(process_page_index,pages-1))
   def table_h = max(60.0*z,gui.remaining_h(34.0*z))
   if previous_page_index != process_page_index {
      gui.scroll_area_ensure_visible("process_scroll", 0.0, 1.0, table_h)
      previous_page_index = process_page_index
   }
   mut bool row_context = false
   if gui.begin_scroll_area("process_scroll", 0.0, table_h) {
      def bool mini = narrow(620.0)
      def bool compact = narrow(1120.0)
      def bool extra = table_mode == 1
      def str tid = mini ? (extra ? "procs_mini_io" : "procs_mini") : (compact ? (extra ? "procs_compact_io" : "procs_compact") : (extra ? "procs_io" : "procs"))
      def cols = mini ? (extra ? [1,0,5,6] : [1,0,3,4]) : (compact ? (extra ? [1,0,4,5,6] : [1,0,2,3,4,7]) : (extra ? [0,1,2,3,4,13,5,6,15,16,14] : [0,1,2,3,4,5,6,7,8,9,10]))
      def labels = mini ? (extra ? ["Process","PID","Read/s","Write/s"] : ["Process","PID","CPU %","RSS"]) : (compact ? (extra ? ["Process","PID","RSS","Read/s","Write/s"] : ["Process","PID","User","CPU %","RSS","State"]) : (extra ? ["PID","Process","User","CPU %","RSS","Virtual","Read/s","Write/s","Read total","Write total","CPU core"] : ["PID","Process","User","CPU %","RSS","Read/s","Write/s","State","Threads","Nice","PPID"]))
      def weights = mini ? [2.6,1.0,1.2,1.3] : (compact ? (extra ? [2.6,0.9,1.2,1.2,1.2] : [2.7,0.85,1.2,1.0,1.2,1.3]) : [1.0,2.8,1.0,0.95,1.15,1.15,1.15,0.7,0.85,0.65,1.0])
      if gui.begin_table(tid, labels, weights) {
         def raw_col = gui.table_sort_column(tid)
         def col = raw_col < 0 ? -1 : int(cols.get(raw_col, -1))
         if col >= 0 && (col != sort_col || gui.table_sort_descending(tid) != descending) {
            sort_col = col
            descending = gui.table_sort_descending(tid)
            rebuild()
         }
         mut int i = process_page_index * per_page
         def end = int(min(visible.len, i + per_page))
         while i < end {
            def entry = visible.get(i, []), p = entry.get(0, [])
            if gui.table_next_row(int(p.get(0, 0)) == selected_pid && int(p.get(11, -1)) == selected_start) {
               if gui.table_row_clicked() {
                  def int pid_now = int(p.get(0, 0)), now_ticks = os.ticks()
                  if pid_now == last_proc_click_pid && now_ticks - last_proc_click_ticks < 450000000 {
                     select_process(p)
                     inspect_selected()
                     last_proc_click_pid = 0
                  } else {
                     select_process(p)
                     last_proc_click_pid = pid_now
                     last_proc_click_ticks = now_ticks
                  }
               }
               if context_hit() { select_process(p) row_context = true }
               mut name = to_str(p.get(1, "")), d = 0
               while d < int(min(12, int(entry.get(1, 0)))) { name = "  " + name d += 1 }
               if compact { gui.table_text(name) }
               gui.table_text(to_str(p.get(0, 0)), "text_dim")
               if !compact { gui.table_text(name) }
               if !mini && (!compact || !extra) { gui.table_text(p.get(2, "?"), "text_dim") }
               if !compact || !extra { gui.table_text(usage(float(p.get(3, -1.0))), float(p.get(3, 0.0)) >= 100.0 ? "warn" : (float(p.get(3, 0.0)) >= 10.0 ? "accent" : "text")) }
               if !mini || !extra { gui.table_text(fmt.fmt_bytes(float(p.get(4, -1)))) }
               if !compact && extra { gui.table_text(fmt.fmt_bytes(float(p.get(13, -1)))) }
               if !compact || extra {
                  gui.table_text(fmt.fmt_rate(float(p.get(5, -1.0))))
                  gui.table_text(fmt.fmt_rate(float(p.get(6, -1.0))))
               }
               if compact {
                  if !mini && !extra { gui.table_text(p.get(7, "?"), "text_dim") }
               } elif extra {
                  gui.table_text(fmt.fmt_bytes(float(p.get(15, -1))))
                  gui.table_text(fmt.fmt_bytes(float(p.get(16, -1))))
                  gui.table_text(to_str(p.get(14, -1)))
               } else {
                  def state = to_str(p.get(7, "?"))
                  gui.table_text(state, state == "R" ? "ok" : (state == "Z" || state == "D" ? "warn" : "text_dim"))
                  gui.table_text(to_str(p.get(8, 0)))
                  gui.table_text(to_str(p.get(9, 0)))
                  gui.table_text(pid_label(p.get(10, -1)), "text_dim")
               }
            }
            i += 1
         }
         gui.end_table()
      }
      if visible.len == 0 {
         gui.spacing(2.0)
         gui.text_colored("No matching processes. Clear or change the filters.", "text_dim")
         if filtered && gui.button("clear_proc_filters", "Clear filters", 110.0 * z, 26.0 * z) {
            query = "" tree_mode = false own_only = false state_filter = 0 filter_changed()
         }
      }
   }
   gui.end_scroll_area()
   if row_context || (menu_key && !inspector_open && selected_pid > 0 && !gui.any_popup_open()) { gui.open_popup("process_actions") }
   process_actions()
   gui.layout_row([0.0,0.0],4.0*z)
   def str selection = selected_row.len > 0 ? to_str(selected_row.get(1,""))+" | PID "+to_str(selected_pid)+" | "+usage(float(selected_row.get(3,-1.0)))+" CPU | "+fmt.fmt_bytes(float(selected_row.get(4,-1))) : "Select a process (double-click to inspect)"
   aligned_label(selection)
   gui.tooltip(selected_command)
   def str live_status = active_status(action_status, action_status_until)
   aligned_label(live_status.len > 0 ? live_status : to_str(visible.len)+" / "+to_str(processes.len)+" tasks | "+to_str(running)+" running")
   gui.tooltip(live_status.len > 0 ? live_status : "Visible / total tasks and running count")
   0
}
fn inspector_window() any {
   if !inspector_open { return 0 }
   if !gui.any_popup_open() { gui.focus_window("inspector") }
   def f64 z = gui.scale()
   def f64 w = float(min(760.0*z,view_w-16.0*z)), h = float(min(570.0*z,view_h-16.0*z))
   def str title = "Process: " + (detail_name.len > 0 ? detail_name + " / " : "") + "PID " + to_str(detail_pid)
   if gui.begin_window("inspector",title,(view_w-w)*0.5,(view_h-h)*0.5,w,h,{"closable":true,"resizable":true,"movable":true,"collapsible":false,"scrollable":false}) {
      gui.layout_row([0.0,84.0*z,92.0*z],4.0*z)
      detail_tab = gui.combo_box("detail_tabs","",["Status","Maps","Files","Threads","I/O","Cgroups","History"],detail_tab)
      gui.begin_disabled(detail_tab == 6 || detail_text.len == 0)
      if ibtn("detail_copy","copy","Copy",false,26.0) { set_action_status(os.set_clipboard_text(detail_text) ? "Details copied" : "Clipboard unavailable") }
      gui.end_disabled()
      gui.begin_disabled(detail_tab == 6)
      if ibtn("detail_refresh","refresh","Refresh",false,26.0) && detail_tab != 6 { detail_refresh() }
      gui.end_disabled()
      if detail_tab != 6 && detail_loaded != detail_tab { detail_refresh() }
      if !identity_matches(detail_pid,detail_start) { gui.text_colored("Process exited or changed identity.","warn") }
      if gui.begin_scroll_area("detail_scroll",0.0,max(50.0*z,gui.remaining_h())) {
         if detail_tab == 6 {
            if watch_pid == detail_pid && watch_start == detail_start {
               def list ch = watch_hist.get("cpu",[]), mh = watch_hist.get("rss",[]), rh = watch_hist.get("read",[]), wh = watch_hist.get("write",[])
               graph("proc_cpu_hist","Process CPU "+usage(last_rate(ch)),ch,float(max(100.0,100.0*float(logical))),blue,100.0)
               graph("proc_rss_hist","Resident memory "+fmt.fmt_bytes(last_rate(mh)),mh,0.0,purple,100.0,"bytes")
               graph("proc_read_hist","Read "+fmt.fmt_rate(last_rate(rh)),rh,0.0,green,100.0)
               graph("proc_write_hist","Write "+fmt.fmt_rate(last_rate(wh)),wh,0.0,orange,100.0)
            } else { gui.text_colored("Process history unavailable.","text_dim") }
         } else {
            if detail_text.len == 0 { gui.text_colored("Unavailable or permission denied","text_dim") }
            mut int i = 0
            while i < detail_lines.len && i < 1000 { gui.text(detail_lines.get(i,"")) i += 1 }
            if i < detail_line_count { gui.text_colored("Output truncated to 1000 lines","text_dim") }
         }
      }
      gui.end_scroll_area()
   }
   gui.end_window()
   if gui.window_closed("inspector") { inspector_open = false gui.focus_window("main") }
   0
}

fn core_meter(int i, list c) any {
   gui.text("")
   def rect = gui.last_item_rect()
   def f64 z = gui.scale()
   def f64 x = float(rect.get(0,0.0)), y = float(rect.get(1,0.0)), w = float(rect.get(2,0.0)), h = float(rect.get(3,0.0))
   def f64 v = float(c.get(1,-1.0))
   def str label = usage(v), name = to_str(c.get(0,""))
   def f64 left = measure_small("cpu"+to_str(logical))+12.0*z, right = measure_small("100%")+12.0*z
   def f64 track = float(max(0.0,w-left-right))
   def f64 ty = y+float(max(0.0,(h-float(gfx.measure_text_fast(font_small,label).get(1,12.0)))*0.5))
   gui.push_clip_rect(rect)
   gui.paint_text(x+3.0*z,ty,name,gui.theme_color("text_dim"),true)
   gui.paint_text(x+w-4.0*z-measure_small(label),ty,label,gui.theme_color("text"),true)
   if track > 5.0*z {
      def f64 bx = x+left, by = y+h*0.5-2.0*z
      gui.paint_rect(bx,by,track,4.0*z,gui.theme_color("surface_active"))
      if v > 0.0 { gui.paint_rect(bx,by,track*float(min(100.0,v))/100.0,4.0*z,v >= 85.0 ? orange : blue) }
   }
   gui.pop_clip_rect()
   gui.tooltip(name+" / "+label+" / "+metric(float(cpu_freq.get(name,-1.0)),"MHz"))
   0
}
fn performance_page() any {
   if gui.begin_scroll_area("performance_scroll", 0.0, max(70.0, gui.remaining_h())) {
      gui.text(cpu_model)
      gui.text_colored(metric(frequency,"MHz")+"   |   "+metric(temperature,"C")+"   |   "+metric(package_power,"W"),"text_dim")
      if !narrow(660.0) { gui.layout_row([0.0, 0.0]) }
      graph("cpu_graph", "CPU " + usage(cpu_usage), cpu_hist, 100.0, blue, 128.0)
      graph("mem_graph", "RAM " + usage(memory_usage), mem_hist, 100.0, purple, 128.0)
      if !narrow(660.0) { gui.layout_row([0.0, 0.0]) }
      graph("swap_graph", "Swap " + fmt.fmt_bytes(float(swap_used)), swap_hist, 100.0, orange, 118.0)
      graph("disk_graph", "Disk read " + fmt.fmt_rate(last_rate(rd_hist)), rd_hist, 0.0, green, 118.0)
      gui.text_colored("Available " + fmt.fmt_bytes(float(max(0, val(memory_details, "MemAvailable"))) * 1024.0) + "  |  Cached " + fmt.fmt_bytes(float(max(0, val(memory_details, "Cached"))) * 1024.0) + "  |  Buffers " + fmt.fmt_bytes(float(max(0, val(memory_details, "Buffers"))) * 1024.0), "text_dim")
      gui.spacing(0.5)
      gui.text("Logical processors / " + to_str(logical))
      gui.separator()
      def int columns = narrow(400.0) ? 1 : (narrow(720.0) ? 2 : (narrow(1120.0) ? 4 : 6))
      mut int i = 0
      while i < cpu_rows.len {
         gui.layout_row(columns == 1 ? [0.0] : (columns == 2 ? [0.0,0.0] : (columns == 4 ? [0.0,0.0,0.0,0.0] : [0.0,0.0,0.0,0.0,0.0,0.0])), 8.0 * gui.scale())
         mut int j = 0
         while j < columns {
            if i < cpu_rows.len {
               def c = cpu_rows.get(i, [])
               core_meter(i,c)
               i += 1
            } else { gui.text("") }
            j += 1
         }
      }
      if frequency >= 0.0 { graph("cpu_freq","CPU frequency "+metric(frequency,"MHz"),freq_hist,0.0,blue,100.0,"MHz") }
      if temperature >= 0.0 { graph("cpu_temp","CPU temperature "+metric(temperature,"C"),temp_hist,100.0,orange,100.0,"C") }
      if package_power >= 0.0 { graph("cpu_power","CPU package power "+metric(package_power,"W"),power_hist,0.0,green,100.0,"W") }
      if gpu_rows.len > 0 { gui.separator() gpu_panel(false) }
   }
   gui.end_scroll_area()
   0
}
fn last_rate(list a) f64 { def int n = int(a.len) n > 0 ? float(a.get(int(n-1),-1.0)) : -1.0 }
fn io_page(bool disk) any {
   def list rows = disk ? disk_rows : net_rows
   mut list names = ["All devices"]
   mut int i = 0, ix = 0
   def str chosen = disk ? disk_choice : net_choice
   while i < rows.len {
      def str n = to_str(rows.get(i,[]).get(0,""))
      names = names.append(n)
      if n == chosen { ix = i+1 }
      i += 1
   }
   ix = gui.combo_box(disk ? "disk_select" : "net_select","Device",names,ix)
   def str selected = ix > 0 ? to_str(names.get(ix,"")) : ""
   if disk { disk_choice = own(selected) } else { net_choice = own(selected) }
   def list hh = device_series.get((disk ? "disk:" : "net:")+selected,[[],[],[]])
   def list a = ix == 0 ? (disk ? rd_hist : rx_hist) : hh.get(0,[])
   def list b = ix == 0 ? (disk ? wr_hist : tx_hist) : hh.get(1,[])
   if !narrow(640.0) { gui.layout_row([0.0, 0.0]) }
   graph("read", (disk ? "Read " : "Receive ") + fmt.fmt_rate(last_rate(a)), a, 0.0, green, 170.0)
   graph("write", (disk ? "Write " : "Transmit ") + fmt.fmt_rate(last_rate(b)), b, 0.0, blue, 170.0)
   if disk { gui.text_colored("Whole block devices; stacked devices can count the same I/O twice.", "text_dim") }
   else { gui.text_colored("All interfaces; aggregate history excludes loopback.", "text_dim") }

   if disk && ix > 0 { graph("busy","Device busy",hh.get(2,[]),100.0,orange,100.0) }
   if ix > 0 {
      def list totals = disk ? disk_prev.get(selected,[]) : net_prev.get(selected,[])
      gui.text_colored("Total in "+fmt.fmt_bytes(float(totals.get(0,-1)))+" / out "+fmt.fmt_bytes(float(totals.get(1,-1))),"text_dim")
   }
   if gui.begin_table(disk ? "disk_io" : "net_io", disk ? ["Device", "Read/s", "Write/s", "Busy", "Capacity"] : ["Interface", "Receive/s", "Transmit/s", "State"], disk ? [1.0, 1.0, 1.0, 1.0, 1.0] : [1.0, 1.0, 1.0, 1.0]) {
      mut int i = 0
      while i < rows.len {
         def r = rows.get(i, [])
         def str dev = to_str(r.get(0, ""))
         if gui.table_next_row(selected == dev) {
            if gui.table_row_clicked() {
               def str next_dev = selected == dev ? "" : dev
               if disk { disk_choice = own(next_dev) } else { net_choice = own(next_dev) }
            }
            gui.table_text(dev)
            gui.table_text(fmt.fmt_rate(float(r.get(1, -1.0))))
            gui.table_text(fmt.fmt_rate(float(r.get(2, -1.0))))
            if disk {
               gui.table_text(usage(float(r.get(3, -1.0))))
               gui.table_text(fmt.fmt_bytes(float(disk_meta.get(dev, -1.0))))
            } else { gui.table_text(net_meta.get(dev, "?")) }
         }
         i += 1
      }
      gui.end_table()
   }
   0
}
mut str mount_choice = ""
fn mount_table() any {
   gui.separator()
   gui.layout_row([0.0, 155.0 * gui.scale()])
   gui.text("Local filesystems")
   gui.begin_disabled(mount_choice.len == 0)
   if ibtn("mount_analyze", "folder", "Analyze usage") { space_path = own(mount_choice) space_start(mount_choice) page = 6 }
   gui.end_disabled()
   if gui.begin_table("mounts", ["Mount point", "Filesystem", "Total", "Available", "Used"], [2.4, 1.0, 1.0, 1.0, 0.9]) {
      mut int i = 0
      while i < mount_rows.len {
         def r = mount_rows.get(i, [])
         def str mp = to_str(r.get(0, ""))
         if gui.table_next_row(mount_choice == mp) {
            if gui.table_row_clicked() {
               def int now_ticks = os.ticks()
               if mp == last_mount_click && now_ticks - last_mount_click_ticks < 450000000 {
                  mount_choice = own(mp)
                  space_path = own(mp)
                  space_start(mp)
                  page = 6
                  last_mount_click = ""
               } else {
                  mount_choice = own(mp)
                  last_mount_click = own(mp)
                  last_mount_click_ticks = now_ticks
               }
            }
            def total = float(r.get(2, -1)), available = float(r.get(3, -1))
            gui.table_text(r.get(0, ""))
            gui.table_text(r.get(1, ""))
            gui.table_text(fmt.fmt_bytes(total))
            gui.table_text(fmt.fmt_bytes(available))
            gui.table_text(total > 0.0 && available >= 0.0 ? fmt.fmt_pct(100.0 * (total - available) / total) : "N/A")
         }
         i += 1
      }
      gui.end_table()
   }
   0
}
fn pressure_page() any {
   gui.text("Pressure stall information")
   gui.text_colored("Time stalled on CPU, memory or I/O; some = at least one task, full = all non-idle tasks.", "text_dim")
   if pressure_rows.len == 0 { gui.text_colored("Kernel PSI counters unavailable.", "text_dim") }
   if gui.begin_table("pressure", ["Resource", "Scope", "10 s %", "60 s %", "300 s %"], [1.2, 1.0, 1.0, 1.0, 1.0]) {
      mut int i = 0
      while i < pressure_rows.len {
         def r = pressure_rows.get(i, [])
         if gui.table_next_row() {
            mut int j = 0
            while j < 5 { gui.table_text(r.get(j, "?")) j += 1 }
         }
         i += 1
      }
      gui.end_table()
   }
   gui.separator()
   gui.text("Memory accounting")
   if gui.begin_table("memory_breakdown", ["Counter", "Size"], [2.0, 1.0]) {
      def keys = ["MemTotal", "MemAvailable", "Active", "Inactive", "AnonPages", "Mapped", "Shmem", "Slab", "SReclaimable", "Dirty", "Writeback", "Committed_AS", "CommitLimit", "SwapTotal", "SwapFree"]
      mut int i = 0
      while i < keys.len {
         def k = to_str(keys.get(i, ""))
         def v = val(memory_details, k)
         if gui.table_next_row() { gui.table_text(k) gui.table_text(fmt.fmt_bytes(v < 0 ? -1.0 : float(v) * 1024.0)) }
         i += 1
      }
      gui.end_table()
   }
   0
}
fn gpu_panel(bool full) any {
   gui.text("Graphics devices / "+to_str(gpu_rows.len))
   if gpu_rows.len == 0 { gui.text_colored("No readable GPU devices.","text_dim") return 0 }
   mut list labels = []
   mut int i = 0, choice = 0
   while i < gpu_rows.len {
      def list g = gpu_rows.get(i,[])
      def str k = gpu_key(g), name = to_str(gpu_metrics.get(k,{}).get("name",""))
      labels = labels.append(to_str(g.get(0,""))+" / "+(name.len > 0 ? name : to_str(g.get(1,""))))
      if gpu_choice == k { choice = i }
      i += 1
   }
   choice = gui.combo_box(full ? "gpu_select" : "gpu_perf_select","",labels,choice)
   def list g = gpu_rows.get(choice,[])
   def str k = gpu_key(g)
   gpu_choice = own(k)
   def dict m = gpu_metrics.get(k,{}), h = gpu_series.get(k,{})
   if !narrow(620.0) { gui.layout_row([0.0,0.0]) }
   graph("gpu_load_"+k,"GPU "+usage(float(m.get("load",-1.0))),h.get("load",[]),100.0,blue,125.0)
   graph("gpu_vram_"+k,"VRAM "+fmt.fmt_bytes(float(m.get("used",-1.0)))+" / "+fmt.fmt_bytes(float(m.get("total",-1.0))),h.get("vram",[]),100.0,purple,125.0)
   if !full { return 0 }
   def list names = ["Temperature","Power","Core clock","Memory clock","Fan","Memory activity"]
   def list keys = ["temp","power","clock","memclock","fan","memload"]
   def list units = ["C","W","MHz","MHz","RPM",""]
   def list ceilings = [100.0,0.0,0.0,0.0,0.0,100.0]
   def list colors = [orange,green,blue,purple,green,purple]
   i = 0
   while i < names.len {
      if i%2 == 0 && !narrow(620.0) { gui.layout_row([0.0,0.0]) }
      def str key = to_str(keys.get(i,""))
      def str unit = to_str(units.get(i,""))
      def f64 v = float(m.get(key,-1.0))
      graph("gpu_"+key+"_"+k,to_str(names.get(i,""))+" "+(unit == "" ? usage(v) : metric(v,unit)),h.get(key,[]),float(ceilings.get(i,0.0)),colors.get(i,blue),110.0,unit)
      i += 1
   }
   if gui.begin_table("gpu_details",["Metric","Reading"],[1.0,1.4]) {
      def list labels = ["Hotspot","Memory temperature","Power limit","Fan duty","GTT used"]
      def list keys = ["hotspot","memtemp","cap","fanpct","gtt"]
      def list units = ["C","C","W","%","bytes"]
      i = 0
      while i < keys.len {
         if gui.table_next_row() {
            def f64 v = float(m.get(keys.get(i,""),-1.0))
            gui.table_text(labels.get(i,""))
            gui.table_text(i == 4 ? fmt.fmt_bytes(v) : metric(v,units.get(i,"")))
         }
         i += 1
      }
      gui.end_table()
   }
   gui.text_colored("Unavailable readings depend on GPU driver and permissions.","text_dim")
   0
}
mut list overview_top = []
mut int overview_stamp = -1
fn overview_page() any {
   if gui.begin_scroll_area("overview_scroll",0.0,max(70.0,gui.remaining_h())) {
      cards()
      if !narrow(640.0) { gui.layout_row([0.0,0.0]) }
      graph("overview_cpu","CPU "+usage(cpu_usage),cpu_hist,100.0,blue,108.0)
      graph("overview_ram","RAM "+usage(memory_usage),mem_hist,100.0,purple,108.0)
      if !narrow(640.0) { gui.layout_row([0.0,0.0]) }
      graph("overview_disk","Disk read "+fmt.fmt_rate(last_rate(rd_hist)),rd_hist,0.0,green,108.0)
      graph("overview_write","Disk write "+fmt.fmt_rate(last_rate(wr_hist)),wr_hist,0.0,orange,108.0)
      if !narrow(640.0) { gui.layout_row([0.0,0.0]) }
      graph("overview_rx","Receive "+fmt.fmt_rate(last_rate(rx_hist)),rx_hist,0.0,green,108.0)
      graph("overview_tx","Transmit "+fmt.fmt_rate(last_rate(tx_hist)),tx_hist,0.0,blue,108.0)
      if gpu_rows.len > 0 { gpu_panel(false) }
      gui.separator()
      gui.text("Top CPU consumers")
      if overview_stamp != proc_stamp { overview_top = own(merge_sort(processes,3,true)) overview_stamp = proc_stamp }
      def list top = overview_top
      if gui.begin_table("overview_processes",["Process","PID","CPU %","RSS"],[2.5,1.0,1.0,1.2]) {
         mut int i = 0
         while i < top.len && i < 12 {
            def list p = top.get(i,[])
            if gui.table_next_row(int(p.get(0,0)) == selected_pid) {
               if gui.table_row_clicked() { select_process(p) page = 0 }
               gui.table_text(p.get(1,""))
               gui.table_text(pid_label(p.get(0,-1)),"text_dim")
               gui.table_text(usage(float(p.get(3,-1.0))))
               gui.table_text(fmt.fmt_bytes(float(p.get(4,-1))))
            }
            i += 1
         }
         gui.end_table()
      }
   }
   gui.end_scroll_area()
   0
}
fn hardware_page() any {
   gpu_panel(true)
   gui.separator()
   gui.text("Battery / power supply")
   def keys = sorted(battery_rows.keys())
   mut int i = 0
   while i < keys.len {
      def str k = to_str(keys.get(i,""))
      def list b = battery_rows.get(k,[])
      gui.text(k+" / "+usage(float(b.get(0,-1.0)))+" / "+to_str(b.get(1,""))+" / "+metric(float(b.get(2,-1.0)),"W"))
      i += 1
   }
   if keys.len == 0 { gui.text_colored("No battery detected.","text_dim") }
   gui.separator()
   gui.text("Temperature / fans / power")
   if sensor_rows.len == 0 { gui.text_colored("No readable hwmon sensors.","text_dim") }
   if gui.begin_table("sensors_table",["Sensor","Reading"],[3.0,1.0]) {
      i = 0
      while i < sensor_rows.len {
         def r = sensor_rows.get(i,[])
         if gui.table_next_row() { gui.table_text(r.get(0,"")) gui.table_text(r.get(1,"")) }
         i += 1
      }
      gui.end_table()
   }
   0
}

mut str space_path = os.home_dir(), space_root = "", space_status = "Choose a folder to analyze."
mut bool space_running = false, space_paused = false, space_dirty = true
mut int space_count = 0, space_files = 0, space_dirs = 0, space_skipped = 0, space_errors = 0, space_links = 0
mut int space_bytes = 0, space_logical = 0, space_started = 0, space_elapsed = 0
mut int space_device = 0, space_mount = 0, space_view = 0, space_metric = 0
mut int space_sort = 2, space_offset = 0, space_press = -1
mut bool space_desc = true
mut str space_filter = "", space_copy_status = ""
mut list space_stack = [], space_rows = [], space_top = [], space_top_logical = [], space_display = []
mut dict space_seen = {}, space_types = {}
mut ptr space_stat = 0

def space_colors = [[0.18, 0.39, 0.65, 1.0], [0.16, 0.48, 0.42, 1.0], [0.39, 0.32, 0.62, 1.0], [0.55, 0.34, 0.19, 1.0], [0.30, 0.43, 0.59, 1.0], [0.42, 0.30, 0.45, 1.0]]
fn space_join(str base, str name) str { base == "/" ? "/" + name : base + "/" + name }
fn space_parent(str path) str {
   def int p = s.find_last(path, "/")
   p <= 0 ? "/" : to_str(slice(path, 0, p))
}
fn space_statx(int fd, str name, int flags) bool {
   mut int call = 332
   #aarch64 { call = 291 } #endif
   sys.syscall(call, fd, name, flags, 0x17ff, space_stat) == 0 && (int(load32(space_stat, 0)) & 0x71f) == 0x71f
}
fn space_dev() int { (int(load32(space_stat, 136)) << 32) | int(load32(space_stat, 140)) }
fn space_mnt() int { (int(load32(space_stat, 0)) & 0x1000) != 0 ? load64_i(space_stat, 144) : 0 }
fn space_dir_open(int fd, str name) int {
   mut int call = 257, flags = 0x0b0000
   #aarch64 { call = 56 flags = 0x80000 | 0xc000 } #endif
   sys.syscall(call, fd, name, flags, 0)
}
fn space_pop() any {
   if space_stack.len == 0 { return 0 }
   def list f = space_stack.get(space_stack.len - 1, [])
   sys.sys_close_quiet(int(f.get(0, -1)))
   def ptr p = f.get(1, 0)
   if bool(p) { free(p) }
   space_stack = slice(space_stack, 0, space_stack.len - 1)
   0
}
fn space_finish(str status) any {
   while space_stack.len > 0 { space_pop() }
   if bool(space_stat) { free(space_stat) space_stat = 0 }
   if space_started > 0 { space_elapsed = os.ticks() - space_started }
   space_running = false
   space_paused = false
   space_status = status
   space_dirty = true
   0
}
fn space_push(int fd, str path, int group) bool {
   def ptr buf = malloc(8192)
   if !buf { sys.sys_close_quiet(fd) return false }
   space_stack = space_stack.append([fd, buf, 0, 0, path, group])
   true
}
fn space_start(str input) bool {
   space_finish("Ready")
   if input.len == 0 { space_status = "Enter a valid directory." return false }
   mut int check = 0
   while check < input.len { if load8(input, check) == 0 { space_status = "Invalid NUL in path." return false } check += 1 }
   mut str path = input
   if load8(path, 0) != 47 { path = space_join(os.getcwd(), path) }
   while path.len > 1 && load8(path, path.len - 1) == 47 { path = slice(path, 0, path.len - 1) }
   def int fd = space_dir_open(-100, path)
   if fd < 0 { space_status = "Cannot open directory (error " + to_str(fd) + ")." return false }
   space_stat = malloc(256)
   if !space_stat || !space_statx(fd, "", 0x1000) {
      sys.sys_close_quiet(fd)
      space_finish("Filesystem metadata unavailable (statx).")
      return false
   }
   space_device = space_dev()
   space_mount = space_mnt()
   space_root = path
   space_path = path
   space_choice = ""
   space_rows = [] space_top = [] space_top_logical = [] space_seen = {} space_types = {} space_display = []
   space_count = 0 space_files = 0 space_dirs = 0 space_skipped = 0 space_errors = 0 space_links = 0
   space_bytes = 0 space_logical = 0 space_offset = 0 space_dirty = true
   space_started = os.ticks() space_elapsed = 0
   space_copy_status = ""
   if !space_push(fd, path, -1) { space_finish("Unable to allocate scan buffer.") return false }
   space_status = "Scanning" space_running = true
   true
}
fn space_top_insert(list rows, list item, int col) list {
   def int weight = item.get(col, 0)
   if rows.len >= 64 && weight <= int(rows.get(63, []).get(col, 0)) { return rows }
   mut int i = 0
   while i < rows.len && int(rows.get(i, []).get(col, 0)) >= weight { i += 1 }
   mut out = slice(rows, 0, i)
   out = out.append(item)
   mut int j = i
   while j < rows.len && out.len < 64 { out = out.append(rows.get(j, [])) j += 1 }
   out
}
fn space_top_add(str path, int allocated, int logical_size) any {
   def list item = [path, false, allocated, logical_size, 1, 0]
   if allocated > 0 { space_top = space_top_insert(space_top, item, 2) }
   if logical_size > 0 { space_top_logical = space_top_insert(space_top_logical, item, 3) }
   0
}
fn space_type(str name, int allocated, int logical_size) any {
   def int p = s.find_last(name, ".")
   mut str ext = p > 0 && p + 1 < name.len ? s.lower(to_str(slice(name, p + 1, name.len))) : "(no extension)"
   if ext.len > 24 || (space_types.len >= 256 && !space_types.contains(ext)) { ext = "(other)" }
   def list old = space_types.get(ext, [0, 0, 0])
   space_types = space_types.set(ext, [int(old.get(0, 0)) + allocated, int(old.get(1, 0)) + logical_size, int(old.get(2, 0)) + 1])
   0
}
fn space_step(int budget=256, int millis=5) any {
   if !space_running || space_paused { return 0 }
   def int deadline = os.ticks() + millis * 1000000
   mut int done = 0
   while space_stack.len > 0 && done < budget {
      if done > 0 && done % 16 == 0 && os.ticks() >= deadline { break }
      if space_count >= 2000000 { space_finish("Entry limit reached; partial results.") return 0 }
      def int idx = space_stack.len - 1
      def list f = space_stack.get(idx, [])
      def int fd = f.get(0, -1)
      def ptr buf = f.get(1, 0)
      mut int pos = f.get(2, 0)
      mut int n = f.get(3, 0)
      def str parent = f.get(4, "")
      mut int group = f.get(5, -1)
      if pos >= n {
         def r = sys.sys_getdents64(fd, buf, 8192)
         if !is_ok(r) { space_errors += 1 space_pop() done += 1 continue }
         n = int(unwrap(r)) pos = 0
         if n == 0 { space_pop() done += 1 continue }
      }
      if n - pos < 20 { space_errors += 1 space_pop() done += 1 continue }
      def int reclen = int(load8(buf, pos + 16)) | (int(load8(buf, pos + 17)) << 8)
      if reclen < 20 || reclen > n - pos { space_errors += 1 space_pop() done += 1 continue }
      def str name = s.cstr_to_str(buf, pos + 19)
      space_stack[idx] = [fd, buf, pos + reclen, n, parent, group]
      done += 1
      if name == "." || name == ".." || name.len == 0 { continue }
      space_count += 1
      if !space_statx(fd, name, 0x900) { space_errors += 1 continue }
      if space_dev() != space_device || (space_mount != 0 && space_mnt() != 0 && space_mnt() != space_mount) { space_skipped += 1 continue }
      def int mode = int(load8(space_stat, 28)) | (int(load8(space_stat, 29)) << 8)
      def int kind = mode & 0xf000
      if kind != 0x4000 && kind != 0x8000 { space_skipped += 1 continue }
      def bool directory = kind == 0x4000
      def str path = space_join(parent, name)
      mut int allocated = load64_i(space_stat, 48) * 512, logical_size = load64_i(space_stat, 40)
      def int inode = load64_i(space_stat, 32)
      if !directory && int(load32(space_stat, 16)) > 1 {
         if space_seen.contains(inode) { space_links += 1 allocated = 0 logical_size = 0 }
         else { space_seen = space_seen.set(inode, true) }
      }
      if group < 0 {
         if space_rows.len >= 16384 { space_finish("Folder item limit reached; partial results.") return 0 }
         group = space_rows.len
         space_rows = space_rows.append([path, directory, 0, 0, 0, 0])
      }
      def list row = space_rows.get(group, [])
      space_rows[group] = [row.get(0, ""), row.get(1, false), int(row.get(2, 0)) + allocated, int(row.get(3, 0)) + logical_size, int(row.get(4, 0)) + (directory ? 0 : 1), int(row.get(5, 0)) + (directory ? 1 : 0)]
      space_bytes += allocated space_logical += logical_size
      if directory {
         space_dirs += 1
         if space_stack.len >= 128 { space_skipped += 1 continue }
         def int child = space_dir_open(fd, name)
         if child < 0 { space_errors += 1 continue }
         if !space_statx(child, "", 0x1000) || space_dev() != space_device || (space_mount != 0 && space_mnt() != 0 && space_mnt() != space_mount) || load64_i(space_stat, 32) != inode {
            sys.sys_close_quiet(child) space_errors += 1 continue
         }
         if !space_push(child, path, group) { space_errors += 1 }
      } else {
         space_files += 1
         space_type(name, allocated, logical_size)
         if allocated > 0 || logical_size > 0 { space_top_add(path, allocated, logical_size) }
      }
   }
   space_elapsed = os.ticks() - space_started
   if space_stack.len == 0 { space_finish(space_errors + space_skipped > 0 ? "Complete with skipped or unreadable entries" : "Complete") }
   0
}
fn space_before(list a, list b, int col, bool desc) bool {
   if col == 0 {
      def str x = s.lower(to_str(a.get(0, "")))
      def str y = s.lower(to_str(b.get(0, "")))
      return desc ? x >= y : x <= y
   }
   def int x = a.get(col, 0)
   def int y = b.get(col, 0)
   if x == y { return to_str(a.get(0, "")) <= to_str(b.get(0, "")) }
   desc ? x > y : x < y
}
fn space_order(list rows, int col, bool desc) list {
   if rows.len < 2 { return rows }
   def int mid = rows.len / 2
   def list a = space_order(slice(rows, 0, mid), col, desc), b = space_order(slice(rows, mid, rows.len), col, desc)
   mut list out = list(rows.len)
   mut int i = 0, j = 0
   while i < a.len || j < b.len {
      if j >= b.len || (i < a.len && space_before(a.get(i, []), b.get(j, []), col, desc)) { out = out.append(a.get(i, [])) i += 1 }
      else { out = out.append(b.get(j, [])) j += 1 }
   }
   out
}
mut list space_map = []
mut str space_choice = ""
mut bool space_choice_dir = false
mut int space_last_ui = 0
fn space_rebuild() any {
   mut list rows = space_view == 0 ? space_rows : (space_metric == 0 ? space_top : space_top_logical)
   if space_view == 2 {
      rows = []
      def keys = space_types.keys()
      mut int i = 0
      while i < keys.len {
         def str key = keys.get(i, "")
         def list r = space_types.get(key, [])
         rows = rows.append([key, false, r.get(0, 0), r.get(1, 0), r.get(2, 0)])
         i += 1
      }
   }
   mut list shown = []
   def str q = s.lower(space_filter)
   mut int i = 0
   while i < rows.len {
      def list r = rows.get(i, [])
      if q.len == 0 || s.find(s.lower(to_str(r.get(0, ""))), q) >= 0 { shown = shown.append(r) }
      i += 1
   }
   space_display = space_order(shown, space_sort, space_desc)
   if space_view == 0 { space_map = space_order(space_rows, space_metric == 0 ? 2 : 3, true) }
   space_dirty = false space_last_ui = os.ticks()
   0
}
fn space_short(str path) str {
   def int p = s.find_last(path, "/")
   p < 0 ? path : to_str(slice(path, p + 1, path.len))
}
fn space_tiles(list rows, int lo, int hi, f64 x, f64 y, f64 w, f64 h, int col, bool hit, f64 mx, f64 my) any {
   if hi <= lo || w < 1.0 || h < 1.0 { return 0 }
   if hi - lo == 1 {
      def list r = rows.get(lo, [])
      def str path = r.get(0, "")
      def bool hover = hit && mx >= x && my >= y && mx < x + w && my < y + h
      def list color = space_colors.get(lo % space_colors.len, blue)
      gui.paint_rect(x + 1.0, y + 1.0, w - 2.0, h - 2.0, path == space_choice ? blue : color)
      if hover {
         gui.paint_rect(x + 1.0, y + 1.0, w - 2.0, 2.0, [0.96, 0.97, 1.0, 1.0])
         gui.paint_rect(x + 1.0, y + h - 3.0, w - 2.0, 2.0, [0.96, 0.97, 1.0, 1.0])
      }
      if w >= 80.0 * gui.scale() && h >= 24.0 * gui.scale() {
         gui.push_clip_rect([x + 4.0, y + 2.0, max(1.0, w - 8.0), max(1.0, h - 4.0)])
         gui.paint_text(x + 7.0, y + 5.0, path == "" ? "Other" : space_short(path), [0.96, 0.97, 1.0, 1.0], true)
         if h >= 48.0 * gui.scale() { gui.paint_text(x + 7.0, y + 23.0 * gui.scale(), fmt.fmt_bytes(float(r.get(col, 0))), [0.90, 0.94, 1.0, 1.0], true) }
         gui.pop_clip_rect()
      }
      if hover {
         gui.tooltip((path == "" ? "Other entries" : path) + " / " + fmt.fmt_bytes(float(r.get(col, 0))))
         if path.len > 0 && gui.mouse_down() && space_press != gui.mouse_press_seq() {
            space_press = gui.mouse_press_seq() space_choice = path space_choice_dir = bool(r.get(1, false))
         }
      }
      return 0
   }
   mut f64 total = 0.0
   mut int i = lo
   while i < hi { total += float(rows.get(i, []).get(col, 0)) i += 1 }
   if total <= 0.0 { return 0 }
   mut int mid = lo + 1
   mut f64 part = float(rows.get(lo, []).get(col, 0))
   while mid < hi - 1 && part < total * 0.5 { part += float(rows.get(mid, []).get(col, 0)) mid += 1 }
   def f64 ratio = part / total
   if w >= h {
      space_tiles(rows, lo, mid, x, y, w * ratio, h, col, hit, mx, my)
      space_tiles(rows, mid, hi, x + w * ratio, y, w * (1.0 - ratio), h, col, hit, mx, my)
   } else {
      space_tiles(rows, lo, mid, x, y, w, h * ratio, col, hit, mx, my)
      space_tiles(rows, mid, hi, x, y + h * ratio, w, h * (1.0 - ratio), col, hit, mx, my)
   }
   0
}
fn space_treemap() any {
   def int col = space_metric == 0 ? 2 : 3
   mut list top = []
   mut int i = 0, rest = 0
   while i < space_map.len {
      def list r = space_map.get(i, [])
      def int n = r.get(col, 0)
      if n > 0 {
         if i < 18 { top = top.append(r) } else { rest += n }
      }
      i += 1
   }
   if rest > 0 { top = top.append(["", false, rest, rest, 0, 0]) }
   if top.len == 0 { return 0 }
   def rect = gui.grid_canvas("space_map", "", 0.0, 156.0 * gui.scale(), 32.0, 4)
   def m = gui.mouse_xy()
   gui.push_clip_rect(rect)
   space_tiles(top, 0, top.len, float(rect.get(0, 0.0)), float(rect.get(1, 0.0)), float(rect.get(2, 0.0)), float(rect.get(3, 0.0)), col, gui.item_hovered(), float(m.get(0, 0.0)), float(m.get(1, 0.0)))
   gui.pop_clip_rect()
   0
}
fn space_page() any {
   def f64 z = gui.scale()
   def bool compact = narrow(760.0)
   gui.layout_row(compact ? [0.0,30.0*z,30.0*z,30.0*z,30.0*z,30.0*z,30.0*z] : [0.0,77.0*z,64.0*z,80.0*z,80.0*z,80.0*z,72.0*z],4.0*z)
   def str entered = gui.input_text("space_path","",space_path,"Directory")
   if entered != space_path { space_path = own(entered) }
   if ibtn("space_home","home",compact ? "" : "Home",false,26.0) { space_path = os.home_dir() }
   gui.tooltip("Home directory")
   if ibtn("space_up","back",compact ? "" : "Up",false,26.0) { space_start(space_parent(space_root.len > 0 ? space_root : space_path)) space_choice = "" }
   gui.tooltip("Scan parent directory")
   if ibtn("space_root","database",compact ? "" : "Root",false,26.0) { space_path = "/" }
   gui.tooltip("Root filesystem")
   if ibtn("space_start","search",compact ? "" : "Scan",false,26.0) { if space_start(space_path) { space_choice = "" } }
   gui.tooltip("Scan selected directory")
   gui.begin_disabled(!space_running)
   if ibtn("space_pause",space_paused ? "play" : "pause",compact ? "" : (space_paused ? "Resume" : "Pause"),space_paused,26.0) { space_paused = !space_paused }
   gui.tooltip("Pause or resume scan")
   if ibtn("space_stop","close",compact ? "" : "Stop",false,26.0) { space_finish("Stopped; partial results.") }
   gui.end_disabled()
   gui.tooltip("Stop scanning")
   if !compact && gui.remaining_h() > 440.0 * z {
   gui.layout_row([0.0, 0.0, 0.0, 0.0], 6.0 * z)
   gui.stat_card("space_alloc", "ALLOCATED", fmt.fmt_bytes(float(space_bytes)), "On-disk blocks", 0.0, 79.0 * z, blue)
   gui.stat_card("space_logical", "LOGICAL", fmt.fmt_bytes(float(space_logical)), "Entry lengths", 0.0, 79.0 * z, purple)
   gui.stat_card("space_files", "FILES", to_str(space_files), to_str(space_links) + " hardlink aliases", 0.0, 79.0 * z, green)
   gui.stat_card("space_folders", "FOLDERS", to_str(space_dirs), to_str(space_errors) + " errors / " + to_str(space_skipped) + " skipped", 0.0, 79.0 * z, orange)
   } else { gui.text_colored(fmt.fmt_bytes(float(space_bytes)) + " allocated / " + to_str(space_files) + " files", "text_dim") }
   gui.text_colored(space_status+" / "+to_str(space_count)+" entries / "+to_str(round(float(space_elapsed)/1000000000.0,1))+" s",space_running ? "accent" : (space_errors > 0 ? "warn" : "text_dim"))
   if space_dirty || (space_running && os.ticks()-space_last_ui > 350000000) { space_rebuild() }
   mut int pages = int(max(1,(space_display.len+99)/100))
   space_offset = int(min(space_offset,pages-1))
   gui.layout_row(compact ? [0.0,94.0*z,90.0*z,58.0*z,30.0*z,30.0*z] : [0.0,145.0*z,145.0*z,80.0*z,32.0*z,32.0*z],4.0*z)
   def str q = gui.input_text("space_filter","",space_filter,"Filter results")
   if q != space_filter { space_filter = own(q) space_dirty = true space_offset = 0 }
   def int v = gui.combo_box("space_view","",compact ? ["Folders","Largest","Types"] : ["Folder items","Largest files","Extensions"],space_view)
   if v != space_view { space_view = v space_dirty = true space_offset = 0 space_choice = "" }
   def int m = gui.combo_box("space_metric","",compact ? ["Allocated","Logical"] : ["Allocated size","Logical size"],space_metric)
   if m != space_metric { space_metric = m space_sort = m == 0 ? 2 : 3 space_desc = true space_dirty = true space_offset = 0 }
   aligned_label(to_str(space_offset+1)+" / "+to_str(pages))
   gui.begin_disabled(space_offset == 0)
   if ibtn("space_prev","back","",false,26.0) { space_offset -= 1 }
   gui.end_disabled()
   gui.begin_disabled(space_offset+1 >= pages)
   if ibtn("space_next","forward","",false,26.0) { space_offset += 1 }
   gui.end_disabled()
   if space_dirty { space_rebuild() }
   pages = int(max(1,(space_display.len+99)/100))
   space_offset = int(min(space_offset,pages-1))
   if space_view == 0 && gui.remaining_h() > 330.0*z { space_treemap() }
   mut bool row_context = false
   if gui.begin_scroll_area("space_rows", 0.0, max(90.0 * z, gui.remaining_h(35.0 * z))) {
      def str tid = "space_table_" + to_str(space_view) + "_" + to_str(space_metric)
      if gui.begin_table(tid, [space_view == 2 ? "Extension" : "Name", "Allocated", "Logical", "Files", "Share"], [3.5, 1.3, 1.3, 0.9, 0.8]) {
         def int raw = gui.table_sort_column(tid)
         def int col = raw == 0 ? 0 : (raw == 1 ? 2 : (raw == 2 ? 3 : (raw == 3 ? 4 : (raw == 4 ? (space_metric == 0 ? 2 : 3) : -1))))
         if col >= 0 && (space_sort != col || space_desc != gui.table_sort_descending(tid)) { space_sort = col space_desc = gui.table_sort_descending(tid) space_dirty = true space_offset = 0 }
         mut int i = space_offset * 100
         def int end = int(min(space_display.len, i + 100))
         while i < end {
            def list r = space_display.get(i, [])
            def str path = r.get(0, "")
            def bool dir = r.get(1, false)
            if gui.table_next_row(space_choice == path) {
               if gui.table_row_clicked() {
                  def int now_ticks = os.ticks()
                  if dir && space_view == 0 && path == last_space_click && now_ticks - last_space_click_ticks < 450000000 {
                     space_start(path)
                     space_choice = ""
                     last_space_click = ""
                  } else {
                     space_choice = own(path)
                     space_choice_dir = dir
                     space_copy_until = 0
                     last_space_click = own(path)
                     last_space_click_ticks = now_ticks
                  }
               }
               if context_hit() { space_choice = own(path) space_choice_dir = dir space_copy_until = 0 row_context = true }
               gui.table_text((dir ? "+  " : "") + (space_view == 0 ? space_short(path) : path))
               gui.table_text(fmt.fmt_bytes(float(r.get(2, 0))))
               gui.table_text(fmt.fmt_bytes(float(r.get(3, 0))))
               gui.table_text(to_str(r.get(4, 0)), "text_dim")
               def int total = space_metric == 0 ? space_bytes : space_logical
               def int bytes = r.get(space_metric == 0 ? 2 : 3, 0)
               gui.table_text(total > 0 ? fmt.fmt_pct(100.0 * float(bytes) / float(total)) : "-", "text_dim")
            }
            i += 1
         }
         gui.end_table()
      }
   }
   gui.end_scroll_area()
   if row_context { gui.open_popup("space_context") }
   if gui.begin_popup("space_context",float(min(210.0*z,view_w-12.0*z))) {
      if gui.menu_item("analyze","Analyze folder","",false,space_choice_dir && space_view == 0) { if space_start(space_choice) { space_choice = "" } }
      if gui.menu_item("copy","Copy path","",false,space_choice.len > 0 && space_view != 2) { set_space_copy_status(os.set_clipboard_text(space_choice) ? "Path copied" : "Clipboard unavailable") }
      gui.end_popup()
   }
   gui.layout_row([0.0,compact ? 32.0*z : 105.0*z,compact ? 32.0*z : 105.0*z],4.0*z)
   def str scopy = active_status(space_copy_status, space_copy_until)
   aligned_label(scopy.len > 0 ? scopy : (space_choice.len > 0 ? space_choice : "Select a folder or file (double-click folder to scan)"))
   gui.tooltip(space_choice)
   gui.begin_disabled(space_choice.len == 0 || !space_choice_dir || space_view != 0)
   if ibtn("space_open", "folder", compact ? "" : "Analyze",false,26.0) { if space_start(space_choice) { space_choice = "" } }
   gui.end_disabled()
   gui.begin_disabled(space_choice.len == 0 || space_view == 2)
   if ibtn("space_copy", "copy", compact ? "" : "Copy path",false,26.0) { set_space_copy_status(os.set_clipboard_text(space_choice) ? "Path copied" : "Clipboard unavailable") }
   gui.end_disabled()
   gui.tooltip("Read-only scan; one filesystem; no symlinks; hardlinks counted once")
   0
}

mut int socket_page = 0, socket_kind = 0, socket_sort = 0
mut bool socket_desc = false, socket_dirty = true
mut str tool_message = "", socket_query = "", socket_selected = ""
mut list socket_rows = [], socket_display = []
mut int socket_stamp = 0, socket_unavailable = 0
fn hexnum(str value) int {
   mut int n = 0, i = 0
   while i < value.len {
      def int c = load8(value, i)
      if c >= 48 && c <= 57 { n = n * 16 + c - 48 }
      elif c >= 65 && c <= 70 { n = n * 16 + c - 55 }
      elif c >= 97 && c <= 102 { n = n * 16 + c - 87 }
      else { return -1 }
      i += 1
   }
   n
}
fn endpoint(str raw) str {
   def int p = s.find(raw, ":")
   if p != 8 && p != 32 { return raw }
   def str port = to_str(hexnum(slice(raw, p + 1, raw.len)))
   if p == 8 {
      mut str out = ""
      mut int i = 6
      while i >= 0 { out += (i == 6 ? "" : ".") + to_str(hexnum(slice(raw, i, i + 2))) i -= 2 }
      return out + ":" + port
   }
   mut str out = "["
   mut int i = 0
   while i < 32 {
      if i > 0 { out += ":" }
      out += to_str(slice(raw, i + 6, i + 8)) + to_str(slice(raw, i + 4, i + 6)) + ":" + to_str(slice(raw, i + 2, i + 4)) + to_str(slice(raw, i, i + 2))
      i += 8
   }
   to_str(s.lower(out)) + "]:" + port
}
fn socket_state(str protocol, str raw) str {
   if protocol == "UDP" || protocol == "UDP6" { return raw == "07" ? "Unconnected" : (raw == "01" ? "Connected" : raw) }
   def states = {"01": "Established", "02": "SYN sent", "03": "SYN received", "04": "FIN wait 1", "05": "FIN wait 2", "06": "Time wait", "07": "Closed", "08": "Close wait", "09": "Last ACK", "0A": "Listening", "0B": "Closing", "0C": "New SYN received"}
   states.get(raw, raw)
}
fn sockets_refresh() any {
   socket_rows = [] socket_unavailable = 0
   def paths = ["tcp", "tcp6", "udp", "udp6"]
   def names = ["TCP", "TCP6", "UDP", "UDP6"]
   mut int i = 0
   while i < paths.len {
      def str protocol = names.get(i, "")
      def str data = proc.read("/proc/net/" + to_str(paths.get(i, "")), 2097152)
      if data.len == 0 { socket_unavailable += 1 i += 1 continue }
      if data.len >= 2097152 { socket_unavailable += 1 }
      def lines = s.split(data, "\n")
      mut int j = 1
      while j < lines.len && socket_rows.len < 20000 {
         def list a = words(to_str(lines.get(j, "")))
         if a.len >= 10 {
            def int uid = num(a.get(7, "0"))
            socket_rows = socket_rows.append([protocol, endpoint(to_str(a.get(1, ""))), endpoint(to_str(a.get(2, ""))), socket_state(protocol, to_str(a.get(3, ""))), to_str(user_names.get(uid, to_str(uid))), to_str(a.get(9, ""))])
         }
         j += 1
      }
      i += 1
   }
   socket_stamp = os.ticks() socket_dirty = true socket_selected = ""
   0
}
fn socket_rebuild() any {
   mut list rows = []
   def str q = s.lower(socket_query)
   mut int i = 0
   while i < socket_rows.len {
      def list r = socket_rows.get(i, [])
      def str protocol = r.get(0, "")
      def bool matches = socket_kind == 0 || (socket_kind == 1 && s.find(protocol, "TCP") == 0) || (socket_kind == 2 && s.find(protocol, "UDP") == 0) || (socket_kind == 3 && to_str(r.get(3, "")) == "Listening")
      if matches && (q.len == 0 || s.find(s.lower(to_str(r)), q) >= 0) { rows = rows.append(r) }
      i += 1
   }
   socket_display = text_rows_sort(rows, socket_sort, socket_desc)
   socket_dirty = false
   0
}
fn text_rows_sort(list rows, int col, bool desc) list {
   if rows.len < 2 { return rows }
   def int mid = rows.len / 2
   def list a = text_rows_sort(slice(rows, 0, mid), col, desc)
   def list b = text_rows_sort(slice(rows, mid, rows.len), col, desc)
   mut list out = list(rows.len)
   mut int i = 0, j = 0
   while i < a.len || j < b.len {
      mut bool left = j >= b.len
      if !left && i < a.len {
         def str x = s.lower(to_str(a.get(i, []).get(col, "")))
         def str y = s.lower(to_str(b.get(j, []).get(col, "")))
         left = desc ? x >= y : x <= y
      }
      if left { out = out.append(a.get(i, [])) i += 1 }
      else { out = out.append(b.get(j, [])) j += 1 }
   }
   out
}
fn system_rows() list {
   def str release = s.strip(proc.read("/proc/sys/kernel/osrelease", 512))
   def str distro = s.strip(proc.read("/etc/os-release", 8192))
   mut str os_name = "Linux"
   def lines = s.split(distro, "\n")
   mut int i = 0
   while i < lines.len {
      def str line = lines.get(i, "")
      if s.find(line, "PRETTY_NAME=") == 0 {
         os_name = slice(line, 12, line.len)
         if os_name.len >= 2 && load8(os_name, 0) == 34 && load8(os_name, os_name.len - 1) == 34 { os_name = slice(os_name, 1, os_name.len - 1) }
         break
      }
      i += 1
   }
   [["Operating system", os_name], ["Kernel", release], ["Hostname", host], ["CPU", cpu_model], ["Logical processors", to_str(logical)], ["Physical memory", fmt.fmt_bytes(float(mem_total))], ["Swap capacity", fmt.fmt_bytes(float(swap_total))], ["Uptime", to_str(round(s.atof(uptime) / 3600.0, 2)) + " hours"], ["Load average", load], ["Processes / threads", to_str(processes.len) + " / " + to_str(threads)], ["User ID", to_str(own_uid)], ["gtop PID", to_str(own_pid)], ["Home", os.home_dir()], ["Working directory", os.getcwd()]]
}
mut list inventory_cache = []
mut int inventory_stamp = -1
fn tools_page() any {
   if tools_view == 0 {
      if gui.begin_scroll_area("system_scroll", 0.0, max(80.0, gui.remaining_h())) {
      if inventory_stamp != last_sample || inventory_cache.len == 0 { inventory_cache = own(system_rows()) inventory_stamp = last_sample }
      def rows = inventory_cache
      gui.layout_row([0.0, 140.0 * gui.scale()])
      gui.text_colored("Live host inventory and quick access", "text_dim")
      if ibtn("tool_copy", "copy", "Copy report") {
         mut str report = "gtop / System information\n"
         mut int i = 0
         while i < rows.len {
            def list r = rows.get(i, [])
            report += to_str(r.get(0, "")) + ": " + to_str(r.get(1, "")) + "\n"
            i += 1
         }
         set_tool_message(os.set_clipboard_text(report) ? "Report copied" : "Clipboard unavailable")
      }
      if gui.begin_table("system_info", ["Property", "Value"], [1.0, 3.0]) {
         mut int i = 0
         while i < rows.len {
            def list r = rows.get(i, [])
            if gui.table_next_row() { gui.table_text(r.get(0, ""), "text_dim") gui.table_text(r.get(1, "")) }
            i += 1
         }
         gui.end_table()
      }
      gui.separator()
      gui.text("Analyze disk usage")
      gui.layout_row([0.0, 0.0, 0.0])
      if ibtn("tool_home", "home", "Home folder") { space_path = os.home_dir() space_start(space_path) page = 6 }
      if ibtn("tool_cwd", "folder", "Working directory") { space_path = os.getcwd() space_start(space_path) page = 6 }
      if ibtn("tool_root", "database", "Root filesystem") { space_path = "/" space_start(space_path) page = 6 }
      gui.text_colored(active_status(tool_message, tool_message_until), "text_dim")
      }
      gui.end_scroll_area()
      return 0
   }
   if socket_stamp == 0 { sockets_refresh() }
   def f64 z = gui.scale()
   def bool compact = narrow(650.0)
   if socket_dirty { socket_rebuild() }
   mut int pages = int(max(1,(socket_display.len+99)/100))
   socket_page = int(min(socket_page,pages-1))
   gui.layout_row(compact ? [0.0,98.0*z,30.0*z,58.0*z,30.0*z,30.0*z] : [0.0,150.0*z,105.0*z,82.0*z,32.0*z,32.0*z],4.0*z)
   def str q = gui.input_text("socket_query","",socket_query,"Filter sockets")
   if q != socket_query { socket_query = own(q) socket_dirty = true socket_page = 0 }
   def int k = gui.combo_box("socket_kind","",["All sockets","TCP","UDP","Listening"],socket_kind)
   if k != socket_kind { socket_kind = k socket_dirty = true socket_page = 0 }
   if ibtn("socket_refresh","refresh",compact ? "" : "Refresh",false,26.0) { sockets_refresh() }
   gui.tooltip("Refresh socket list")
   aligned_label(to_str(socket_page+1)+" / "+to_str(pages))
   gui.begin_disabled(socket_page == 0)
   if ibtn("socket_prev","back","",false,26.0) { socket_page -= 1 }
   gui.end_disabled()
   gui.begin_disabled(socket_page+1 >= pages)
   if ibtn("socket_next","forward","",false,26.0) { socket_page += 1 }
   gui.end_disabled()
   if socket_dirty { socket_rebuild() }
   pages = int(max(1,(socket_display.len+99)/100))
   socket_page = int(min(socket_page,pages-1))
   mut bool row_context = false
   if gui.begin_scroll_area("socket_scroll", 0.0, max(90.0, gui.remaining_h(65.0 * gui.scale()))) {
      if gui.begin_table("sockets", ["Protocol", "Local endpoint", "Remote endpoint", "State", "User", "Inode"], [0.8, 2.5, 2.5, 1.2, 1.1, 1.0]) {
         def int col = gui.table_sort_column("sockets")
         if col >= 0 && (col != socket_sort || socket_desc != gui.table_sort_descending("sockets")) { socket_sort = col socket_desc = gui.table_sort_descending("sockets") socket_dirty = true socket_page = 0 }
         mut int i = socket_page * 100
         def int end = int(min(socket_display.len, i + 100))
         while i < end {
            def list r = socket_display.get(i, [])
            def str selected = to_str(r)
            if gui.table_next_row(socket_selected == selected) {
               if gui.table_row_clicked() || context_hit() { socket_selected = own(selected) if context_hit() { row_context = true } }
               mut int j = 0
               while j < 6 { gui.table_text(r.get(j, ""), j == 3 && to_str(r.get(j, "")) == "Listening" ? "accent" : "text") j += 1 }
            }
            i += 1
         }
         gui.end_table()
      }
   }
   gui.end_scroll_area()
   if row_context { gui.open_popup("socket_context") }
   if gui.begin_popup("socket_context",float(min(210.0*z,view_w-12.0*z))) {
      if gui.menu_item("copy","Copy socket","",false,socket_selected.len > 0) { set_tool_message(os.set_clipboard_text(socket_selected) ? "Socket copied" : "Clipboard unavailable") }
      if gui.menu_item("refresh","Refresh sockets") { sockets_refresh() }
      gui.end_popup()
   }
   gui.layout_row([0.0, 140.0 * gui.scale()])
   gui.text_colored(socket_selected.len > 0 ? socket_selected : "Current network namespace; no PID attribution.", "text_dim")
   gui.tooltip(socket_selected)
   gui.begin_disabled(socket_selected.len == 0)
   if ibtn("socket_copy", "copy", "Copy socket") { set_tool_message(os.set_clipboard_text(socket_selected) ? "Socket copied" : "Clipboard unavailable") }
   gui.end_disabled()
   def str tmsg = active_status(tool_message, tool_message_until)
   gui.text_colored(to_str(socket_unavailable) + " unavailable or truncated sources / max 20,000 sockets / " + to_str(round(float(os.ticks() - socket_stamp) / 1000000000.0, 1)) + " s since refresh" + (tmsg.len > 0 ? " / " + tmsg : ""), socket_unavailable > 0 ? "warn" : "text_dim")
   0
}

#linux {} #else { panic("gtop currently requires Linux") } #endif
def str space_probe = common.env_trim("NY_GTOP_SPACE_PROBE")
if space_probe.len > 0 {
   assert(space_start(space_probe), space_status)
   while space_running { space_step(4096, 100) }
   print("GTOP_SPACE " + to_str(space_bytes) + " " + to_str(space_logical) + " files=" + to_str(space_files) + " dirs=" + to_str(space_dirs) + " aliases=" + to_str(space_links) + " skipped=" + to_str(space_skipped) + " errors=" + to_str(space_errors))
   print("GTOP_SPACE_ROWS " + to_str(space_rows))
   print("GTOP_SPACE_TYPES " + to_str(space_types))
   os.exit(0)
}
if common.env_truthy("NY_GTOP_PROBE") {
   def str root = common.env_trim("NY_GTOP_GPU_FIXTURE")
   if root.len > 0 {
      def dict m = gpu_sample(root+"/",["card0","test",73.0,1024.0,2048.0])
      assert(float(m.get("vram",-1.0)) == 50.0,"GPU memory percent")
      assert(float(m.get("temp",-1.0)) == 55.0,"GPU temperature")
      assert(float(m.get("power",-1.0)) == 125.0,"GPU power")
      assert(float(m.get("clock",-1.0)) == 1800.0,"GPU clock")
      assert(float(m.get("fan",-1.0)) == 1200.0,"GPU fan")
      assert(float(m.get("hotspot",-1.0)) == 70.0,"GPU hotspot")
      print("GTOP_GPU_OK")
      if common.env_truthy("NY_GTOP_NVML_PROBE") {
         def dict nv = nvml_metrics("0000:01:00.0",m)
         assert(to_str(nv.get("name","")) == "NVML fixture","NVML name")
         assert(float(nv.get("load",-1.0)) == 67.0,"NVML load")
         assert(float(nv.get("vram",-1.0)) == 25.0,"NVML VRAM")
         assert(float(nv.get("power",-1.0)) == 100.0,"NVML power")
         assert(float(nv.get("memclock",-1.0)) == 7000.0,"NVML memory clock")
         nvml_stop()
         print("GTOP_NVML_OK")
      }
   }
   collect()
   assert(to_str(proc.pid_stat("42 (name ) with spaces) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21").get(1, "")) == "name ) with spaces")
   assert(proc.kv_int("Uid:\t1000 1000\nVmRSS:   42 kB\n", "Uid") == 1000 && proc.kv_int("Uid:\t1000 1000\nVmRSS:   42 kB\n", "VmRSS") == 42 && proc.kv_int("notUid: 5", "Uid") == -1, "process counter parsing")
   assert(endpoint("0100007F:1F90") == "127.0.0.1:8080", "IPv4 endpoint")
   assert(endpoint("00000000000000000000000001000000:0016") == "[0000:0000:0000:0000:0000:0000:0000:0001]:22", "IPv6 endpoint")
   assert(cpu_prev.contains("cpu"), "missing CPU counters")
   assert(state_matches("Z", 4) && !state_matches("Z", 2), "state filter")
   assert(before([1, "", "", 1.0], [2, "", "", -1.0], 3, false), "unknown values sorted first")
   assert(proc.read("/proc/self/stat", 16).len == 16, "bounded read")
   assert(identity_matches(own_pid, proc.pid_stat(proc.read("/proc/self/stat")).get(7, -1)), "identity check")
   assert(mem_total > 0, "missing memory counters")
   assert(processes.len > 0, "missing processes")
   os.msleep(150)
   def int prior_count = processes.len
   collect_begin()
   collect_step(1,1)
   assert(!scan_active || processes.len == prior_count,"partial process snapshot leaked")
   while scan_active { collect_step(2,5) }
   print("GTOP_INCREMENTAL_OK")
   rebuild()
   assert(visible.len == processes.len, "process order lost entries")
   assert(cpu_usage >= 0.0 && cpu_usage <= 100.0, "invalid CPU rate")
   tree_mode = true
   rebuild()
   assert(visible.len == processes.len, "process tree lost entries")
   tree_mode = false
   query = "999999999999999999"
   rebuild()
   assert(visible.len == 0, "filter failed")
   def capacity = proc.fs_capacity("/")
   print("GTOP_STORAGE " + to_str(capacity))
   assert(int(capacity.get(0, -1)) > 0 && int(capacity.get(1, -1)) >= 0, "filesystem capacity failed")
   def exe = proc.readlink("/proc/self/exe")
   assert(exe.len > 0, "readlink failed")
   print("GTOP_EXE " + exe)
   def test_pid = common.env_int_clamped("NY_GTOP_TEST_PID", 0, 0, 2147483647)
   if test_pid > 1 {
      def st = proc.pid_stat(proc.read("/proc/" + to_str(test_pid) + "/stat"))
      assert(st.len == 10, "test child missing")
      def int start = st.get(7, -1)
      assert(s.find(send_signal(test_pid, start + 1, 0), "Signal sent") < 0, "stale identity accepted")
      assert(s.find(send_signal(test_pid, start, 0), "Signal sent") == 0, "pidfd signal probe failed")
   }
   print("GTOP_OK tasks=" + to_str(processes.len) + " cores=" + to_str(logical) + " sample_ms=" + to_str(sample_ms))
   nvml_stop()
   os.exit(0)
}
gfx.apply_backend_env()
gfx.apply_backend_argv()
bootstrap.apply_identity_hints("Gtop", "gtop")
def bool offscreen = gfx.get_backend_type() == int(gfx.BACKEND_MOCK) || common.env_truthy("NY_UI_HEADLESS") || common.env_truthy("NYTRIX_UI_REPLAY")
fn startup_error(str reason) any {
   eprint("gtop: " + reason)
   eprint("  platform=" + window.backend() + "  renderer=" + gfx.get_active_backend_name())
   eprint("  WAYLAND_DISPLAY=" + common.env_trim("WAYLAND_DISPLAY") + "  DISPLAY=" + common.env_trim("DISPLAY"))
   eprint("  XDG_RUNTIME_DIR=" + common.env_trim("XDG_RUNTIME_DIR") + "  NY_UI_BACKEND=" + common.env_trim("NY_UI_BACKEND"))
   eprint("  Run from a graphical session as your desktop user. For Wayland: NY_UI_BACKEND=wayland NY_UI_RENDER_BACKEND=gl")
   eprint("  Set NY_UI_DEBUG_WINDOW=1 to diagnose platform initialization; NY_GTOP_PROBE=1 tests without a window.")
   os.exit(1)
}
if !offscreen && !common.env_present("WAYLAND_DISPLAY") && !common.env_present("DISPLAY") {
   startup_error("No graphical display in the session environment.")
}
if common.env_truthy("NY_GTOP_TRACE") { eprint("gtop: initializing window") }
def win = gfx.init_window(1280, 820, "gtop | Task Manager", consts.WINDOW_CENTER | consts.WINDOW_FOCUS_ON_SHOW, false, true, 1)
if !win { startup_error(gfx.window_error()) }
if !offscreen && gfx.get_active_backend() == int(gfx.BACKEND_MOCK) {
   def str reason = gfx.window_error()
   gfx.close_window()
   startup_error(reason.len > 0 ? reason : "No visible graphics renderer initialized.")
}
window.set_exit_key(win, consts.KEY_NULL)
if common.env_truthy("NY_GTOP_TRACE") { eprint("gtop: window ready") }
collect_begin()
if common.env_truthy("NY_GTOP_TRACE") { eprint("gtop: initial sample started") }
dirty = true
mut font_scale = 0.0
mut int frame = 0
mut int surface_failures = 0
mut bool refresh_requested = false, manual_scan = false
def fonts = extend(["etc/assets/fonts/maplemono.ttf", "etc/assets/fonts/jetbrains.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"], viewer.FONT_CANDIDATES)
def heading_fonts = fonts
load_icons()
if common.env_truthy("NY_GTOP_TRACE") { eprint("gtop: icon textures ready") }
tools_view = common.env_int_clamped("NY_GTOP_TOOLS_VIEW", 0, 0, 1)
def str space_initial = common.env_trim("NY_GTOP_SCAN")
if space_initial.len > 0 { space_start(space_initial) }
palette(common.env_trim("NY_GTOP_THEME") == "light" ? "light" : "dark")
def int frame_limit = common.env_int_clamped("NY_GTOP_FRAMES", 0, 0, 1000000)
def snapshot = common.env_trim("NY_GTOP_SNAPSHOT")
def intervals = [500, 1000, 2000, 5000]
mut int last_esc_ticks = 0
mut bool quit_requested = false
while !gfx.window_should_close(win) && !quit_requested {
   secondary_pressed = false
   menu_key = false
   mut bool refresh_now = refresh_requested
   refresh_requested = false
   mut int events = 0
   while events < 128 {
      def e = window.check_event(win)
      if is_nil(e) || (is_int(e) && int(e) == 0) { break }
      events += 1
      def typ = window.event_type(e), data = window.event_data(e)
      if typ == EVENT_MOUSE_BUTTON_PRESSED && int(data.get("button",-1)) == 1 { secondary_pressed = true }
      if typ == EVENT_FOCUS_OUT { secondary_pressed = false menu_key = false gui.close_all_popups() }
      if typ == EVENT_KEY_PRESSED {
         def int key = int(data.get("key", 0))
         if key == KEY_MENU { menu_key = true }
         elif key == KEY_F5 { refresh_now = true }
         elif key == KEY_F8 { paused = !paused }
         elif key == KEY_ESCAPE {
            if inspector_open && !gui.any_popup_open() {
               inspector_open = false gui.show_window("inspector",false) gui.focus_window("main") last_esc_ticks = 0
            } elif !gui.any_popup_open() && !inspector_open && !gui.wants_keyboard() {
               def int now_ticks = os.ticks()
               if last_esc_ticks > 0 && now_ticks - last_esc_ticks < 450000000 { quit_requested = true }
               else { last_esc_ticks = now_ticks set_action_status("Press Esc again to exit") }
            }
         }
         elif !gui.any_popup_open() && !inspector_open {
            if key >= KEY_F1 && key <= KEY_F4 { page = key - KEY_F1 }
            elif key == KEY_F6 { page = 4 }
            elif key == KEY_F7 { page = 5 }
            elif key == KEY_F9 { page = 6 }
            elif key == KEY_F10 { page = 7 tools_view = 0 }
            elif key == KEY_F11 { page = 8 }
            elif page == 0 && !gui.wants_keyboard() {
               if key == KEY_UP { step_process_selection(-1) }
               elif key == KEY_DOWN { step_process_selection(1) }
               elif key == KEY_PAGE_UP && process_page_index > 0 { process_page_index -= 1 }
               elif key == KEY_PAGE_DOWN {
                  def int per_page = int(page_sizes.get(page_size_index, 50))
                  def int pages = int(max(1, (visible.len + per_page - 1) / per_page))
                  if process_page_index + 1 < pages { process_page_index += 1 }
               }
               elif key == KEY_ENTER && selected_pid > 0 { inspect_selected() }
               elif key == KEY_DELETE && selected_pid > 1 && selected_pid != own_pid { queue_signal(15, "Terminate") }
            }
         }
      }
      gui.feed_event(typ, data)
   }
   if refresh_now { manual_scan = true collect_begin() }
   if paused && !manual_scan && scan_active { collect_cancel() }
   if !paused && !scan_active && os.ticks()-last_sample >= int(intervals.get(interval,1000))*1000000 && os.ticks()-scan_completed >= int(max(150, int(intervals.get(interval,1000))/4))*1000000 { collect_begin() }
   if scan_active && (!paused || manual_scan) {
      if collect_step() { dirty = true manual_scan = false }
   }
   space_step()
   def fb = gfx.framebuffer_size_f64(1280, 820)
   def f64 sw = float(fb.get(0, 1280)), sh = float(fb.get(1, 820))
   if sw <= 0.0 || sh <= 0.0 { os.msleep(32) continue }
   if sw != view_w || sh != view_h { gui.close_all_popups() }
   view_w, view_h = sw, sh
   def f64 raw_dpi = float(max(0.75, float(window.get_window_content_scale(win).get(0, 1.0))))
   def f64 preferred = float(min(1.20, max(0.75, raw_dpi * 0.90)))
   def f64 fitted = float(max(0.75, min(preferred,min(sw/560.0,sh/440.0))))
   def f64 dpi = float(round(fitted*8.0,0))/8.0
   def f64 font_dpi = dpi
   if font_dpi != font_scale {
      def int nf = gfx.font_load_first(fonts, int(16.0 * font_dpi + 0.5))
      def int nt = gfx.font_load_first(heading_fonts, int(24.0 * font_dpi + 0.5))
      def int ns = gfx.font_load_first(fonts, int(14.0 * font_dpi + 0.5))
      if nf > 0 && nt > 0 && ns > 0 {
         if font_body > 0 { gfx.font_destroy(font_body) }
         if font_title > 0 { gfx.font_destroy(font_title) }
         if font_small > 0 { gfx.font_destroy(font_small) }
         font_body, font_title, font_small, font_scale = nf, nt, ns, font_dpi
      } else {
         if nf > 0 { gfx.font_destroy(nf) }
         if nt > 0 { gfx.font_destroy(nt) }
         if ns > 0 { gfx.font_destroy(ns) }
         font_scale = font_dpi
      }
   }
   gui.set_enabled(true)
   gui.set_scale(dpi)
   gui.set_fonts(font_body, font_title, font_small)
   gui.prepare_input(win, sw, sh)
   if !gfx.begin_frame_clear(gui.theme_color("window")) {
      surface_failures += 1
      if surface_failures >= 180 { eprint("gtop: graphics surface unavailable") break }
      os.msleep(32)
      continue
   }
   gfx.set_ortho_2d(0.0, sw, sh, 0.0)
   gui.begin_frame(0, font_body, sw, sh)
   gui.apply_window_rect("main", [0.0, 0.0, sw, sh])
   if gui.begin_window("main", "gtop / " + host, 0.0, 0.0, sw, sh, {"closable": false, "collapsible": false, "resizable": false, "movable": false, "titlebar": false, "scrollable": false}) {
      gui.begin_disabled(inspector_open)
      def bool compact = narrow(840.0)
      def str status = scan_active ? "Scanning "+to_str(scan_index)+"/"+to_str(scan_pids.len) : (paused ? "Paused" : (scan_wall_ms > float(intervals.get(interval,1000)) ? "Slow" : "Live"))
      if compact { gui.layout_row([0.0,30.0*dpi,30.0*dpi,30.0*dpi,30.0*dpi],4.0*dpi) }
      else { gui.layout_row([96.0*dpi,106.0*dpi,122.0*dpi,106.0*dpi,0.0,30.0*dpi,30.0*dpi,30.0*dpi,30.0*dpi],4.0*dpi) }
      page = navigation(page,compact)
      if !compact {
         aligned_label(status+" / "+to_str(round(sample_ms,1))+" ms")
         gui.tooltip("Scan work / wall time "+to_str(round(scan_wall_ms,1))+" ms")
      }
      if ibtn("pause",paused ? "play" : "pause","",paused,26.0) { paused = !paused }
      gui.tooltip(status+" / Pause or resume sampling (F8)")
      if ibtn("refresh","refresh","",false,26.0) { refresh_requested = true }
      gui.tooltip("Refresh now (F5)")
      if ibtn("theme_toggle",gui.theme() == "dark" ? "sun" : "moon","",false,26.0) { palette(gui.theme() == "dark" ? "light" : "dark") }
      gui.tooltip("Toggle light/dark theme")
      if ibtn("settings","gear","",false,26.0) { menu_anchor("settings_popup") }
      gui.tooltip("Sampling interval and theme")
      page = navigation_popups(page,compact)
      settings_menu()
      gui.separator()
      if page == 0 && !compact && gui.remaining_h() > 470.0*dpi { cards() }
      elif page == 0 { gui.text_colored("CPU "+usage(cpu_usage)+"  /  RAM "+usage(memory_usage)+"  /  "+to_str(processes.len)+" tasks","text_dim") }
      if page == 0 { process_page() }
      elif page == 1 { performance_page() }
      elif page == 6 { space_page() }
      elif page == 7 { tools_page() }
      elif page == 8 { overview_page() }
      else {
         if gui.begin_scroll_area("resource_page_" + to_str(page), 0.0, max(80.0, gui.remaining_h())) {
            if page == 2 { io_page(true) mount_table() }
            elif page == 3 { io_page(false) }
            elif page == 4 { hardware_page() }
            else { pressure_page() }
         }
         gui.end_scroll_area()
      }
      action_popup()
      gui.end_disabled()
   }
   gui.end_window()
   inspector_window()
   gui.end_frame()
   frame += 1
   if frame_limit > 0 && frame >= frame_limit && snapshot.len > 0 { gfx.snapshot(snapshot) }
   if frame_limit > 0 && frame >= frame_limit { print("GTOP_GUI page=" + to_str(page) + " frames=" + to_str(frame) + " cpu=" + to_str(cpu_usage)) }
   if !gfx.end_frame() {
      surface_failures += 1
      if surface_failures >= 180 { eprint("gtop: presentation failed") break }
      os.msleep(32)
   } else {
      surface_failures = 0
      if frame == 1 && common.env_truthy("NY_GTOP_TRACE") { eprint("gtop: first frame presented") }
   }
   if frame_limit > 0 && frame >= frame_limit { break }
   os.msleep(scan_active ? 25 : 40)
}
collect_cancel()
space_finish("Closed")
nvml_stop()
if font_body > 0 { gfx.font_destroy(font_body) }
if font_title > 0 { gfx.font_destroy(font_title) }
if font_small > 0 { gfx.font_destroy(font_small) }
free_icons()
gfx.close_window()
os.exit(surface_failures >= 180 ? 1 : 0)
