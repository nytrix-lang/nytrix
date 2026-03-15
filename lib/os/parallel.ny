;; Keywords: parallel threads scheduler work-stealing
;; Parallel CPU threading policy.
module std.os.parallel(parallel_mode, parallel_threads, parallel_min_work, parallel_should_threads, parallel_status, hardware_threads, thread_budget, future, async, detach, future_wait, parallel_map, parallel_map_indexed, parallel_each, chunk_ranges, scheduler_policy, scheduler_status, work_stealing_enabled, work_stealing_plan, work_queue, work_queue_push, work_queue_pop, work_queue_steal, PARALLEL_MODE, PARALLEL_THREADS, PARALLEL_MIN_WORK, SCHEDULER_POLICY, HARDWARE_THREADS)
use std.core
use std.core.str
use std.os.prim
use std.os.info as osinfo
use std.core.common as common
use std.os.thread

fn _normalize_parallel_mode(any: v): str {
   if(!is_str(v)){ return "auto" }
   def s = lower(strip(v))
   case s {
      "off", "auto", "threads" -> s
      _ -> "auto"
   }
}

fn _normalize_scheduler_policy(any: v): str {
   if(!is_str(v)){ return "auto" }
   def s = lower(strip(v))
   case s {
      "off", "direct", "auto", "work-stealing" -> s
      "work_stealing" -> "work-stealing"
      _ -> "auto"
   }
}

fn _logical_cpu_guess(): int {
   def n1 = common.parse_nonneg_int(env("NYTRIX_LOGICAL_CPUS"))
   if(n1 > 0){ return n1 }
   def n2 = common.parse_nonneg_int(env("NUMBER_OF_PROCESSORS"))
   if(n2 > 0){ return n2 }
   def n3 = common.parse_nonneg_int(env("NPROC"))
   if(n3 > 0){ return n3 }
   def n4 = osinfo.cpu_logical_count()
   if(n4 > 0){ return n4 }
   2
}

mut _parallel_threads_eff_loaded = false
mut _parallel_threads_eff_cache = 0
mut _parallel_min_work_eff_loaded = false
mut _parallel_min_work_eff_cache = 0

fn _effective_parallel_threads(): int {
   if(_parallel_threads_eff_loaded){ return _parallel_threads_eff_cache }
   mut out = 2
   if(PARALLEL_THREADS > 0){ out = PARALLEL_THREADS }
   else {
      def n = _logical_cpu_guess()
      if(n > 1){ out = n }
   }
   _parallel_threads_eff_cache = out
   _parallel_threads_eff_loaded = true
   out
}

fn _effective_parallel_min_work(): int {
   if(_parallel_min_work_eff_loaded){ return _parallel_min_work_eff_cache }
   mut out = 65536
   if(PARALLEL_MIN_WORK > 0){ out = PARALLEL_MIN_WORK }
   else {
      def t = _effective_parallel_threads()
      if(t >= 8){ out = 262144 }
      elif(t >= 4){ out = 131072 }
   }
   _parallel_min_work_eff_cache = out
   _parallel_min_work_eff_loaded = true
   out
}

def PARALLEL_MODE = _normalize_parallel_mode(strip(to_str(env("NYTRIX_PARALLEL_MODE"))))
def PARALLEL_THREADS = common.parse_nonneg_int(env("NYTRIX_PARALLEL_THREADS"))
def PARALLEL_MIN_WORK = common.parse_nonneg_int(env("NYTRIX_PARALLEL_MIN_WORK"))
def SCHEDULER_POLICY = _normalize_scheduler_policy(strip(to_str(env("NYTRIX_THREAD_SCHEDULER"))))

fn parallel_mode(): str {
   "Returns the configured parallel mode: `off`, `auto`, or `threads`.
   Configure via compiler CLI flag `--parallel`."
   PARALLEL_MODE
}

fn parallel_threads(): int {
   "Returns configured thread budget; `0` means runtime/default auto sizing.
   Configure via compiler CLI flag `--threads`."
   PARALLEL_THREADS
}

fn parallel_min_work(): int {
   "Returns minimum work threshold before selecting threaded parallel execution.
   Configure via compiler CLI flag `--parallel-min-work`."
   PARALLEL_MIN_WORK
}

fn _parallel_status_reason(str: mode, int: threads_eff, int: work_items, int: min_work_eff): str {
   case mode {
      "off" -> "parallel_mode_off"
      _ -> (threads_eff <= 1 ? "single_thread_budget" :
      ((work_items > 0 && work_items < min_work_eff) ? "below_min_work" : "eligible"))
   }
}

fn parallel_status(int: work_items=0): dict {
   "Returns a threading decision map for `work_items`."
   def threads_eff = _effective_parallel_threads()
   def min_work_eff = _effective_parallel_min_work()
   def reason = _parallel_status_reason(PARALLEL_MODE, threads_eff, work_items, min_work_eff)
   {"mode": PARALLEL_MODE, "threads": PARALLEL_THREADS, "effective_threads": threads_eff,
      "min_work": PARALLEL_MIN_WORK, "effective_min_work": min_work_eff, "work_items": work_items,
   "selected": reason == "eligible", "reason": reason}
}

fn parallel_should_threads(int: work_items=0): bool {
   "Returns true when thread-parallel policy selects threaded execution."
   parallel_status(work_items).get("selected", false)
}

fn scheduler_policy(): str {
   "Returns the configured @thread scheduler policy: `auto`, `direct`, or `work-stealing`."
   SCHEDULER_POLICY
}

fn work_stealing_enabled(int: work_items=0): bool {
   "Returns true when the thread runner should use work-stealing queues for this workload."
   case scheduler_policy(){
      "off", "direct" -> false
      "work-stealing" -> true
      _ -> parallel_should_threads(work_items) && thread_budget(work_items) > 1
   }
}

fn _active_scheduler_policy(str: configured, bool: stealing): str {
   case configured {
      "auto" -> stealing ? "work-stealing" : "direct"
      _ -> configured
   }
}

fn scheduler_status(int: work_items=0): dict {
   "Returns scheduler metadata used by high-level @thread and parallel helpers."
   def pst = parallel_status(work_items)
   def stealing = work_stealing_enabled(work_items)
   def active_policy = _active_scheduler_policy(scheduler_policy(), stealing)
   pst.merge({"scheduler": active_policy, "configured_scheduler": SCHEDULER_POLICY,
   "work_stealing": stealing, "runner": "@thread"})
}

fn hardware_threads(): int {
   "Returns the effective logical CPU thread budget selected by std.os.parallel."
   def n = parallel_status(0).get("effective_threads", 1)
   if(n > 0){ return n }
   1
}

fn thread_budget(int: work_items=0, int: max_threads=0): int {
   "Returns a bounded worker count for `work_items` and optional `max_threads`."
   def base = parallel_status(work_items).get("effective_threads", 1)
   def limited = (max_threads > 0 && max_threads < base) ? max_threads : base
   def capped = (work_items > 0 && work_items < limited) ? work_items : limited
   capped < 1 ? 1 : capped
}

fn future(fnptr: work, any: arg=0): any {
   "Starts `work(arg)` on a joinable worker thread."
   thread_spawn(work, arg)
}

fn async(fnptr: work, any: arg=0): any {
   "Alias for future(work, arg)."
   future(work, arg)
}

fn detach(fnptr: work, any: arg=0): any {
   "Starts `work(arg)` on a detached worker thread."
   thread_launch(work, arg)
}

fn future_wait(any: thread_handle): any {
   "Waits for a future returned by `future` or `async`."
   thread_join(thread_handle)
}

fn chunk_ranges(int: count, int: workers): list {
   "Returns `[start, stop]` ranges splitting `count` items over `workers` chunks."
   mut out = list()
   if(count <= 0){ return out }
   if(workers < 1){ workers = 1 }
   if(workers > count){ workers = count }
   def chunk = (count + workers - 1) / workers
   mut start = 0
   while(start < count){
      mut stop = start + chunk
      if(stop > count){ stop = count }
      out = out.append([start, stop])
      start = stop
   }
   out
}

fn work_queue(int: id=0): dict {
   "Creates a scheduler work queue."
   Queue().merge({"kind": "work-queue", "id": id})
}

fn work_queue_push(dict: q, any: task): dict {
   "Pushes a task onto a scheduler work queue."
   queue_push(q, task)
}

fn work_queue_pop(dict: q): dict {
   "Pops a task from the owner queue."
   mut r = queue_try_pop(q)
   r = r.set("from", q.get("id", 0))
   r
}

fn work_queue_steal(list: queues, int: victim=0): dict {
   "Attempts to steal a task from another queue and returns `{ok, value, from}`."
   if(queues.len == 0){ return {"ok": false, "value": 0, "from": -1} }
   mut i = 0
   while(i < queues.len){
      def idx = (victim + i) % queues.len
      def q = queues.get(idx)
      if(queue_len(q) > 0){
         def r = queue_try_pop(q)
         return {"ok": r.get("ok", false), "value": r.get("value", 0), "from": q.get("id", idx)}
      }
      i += 1
   }
   {"ok": false, "value": 0, "from": -1}
}

fn work_stealing_plan(int: work_items=0, int: max_threads=0): dict {
   "Returns queue and chunk metadata for work-stealing thread execution."
   def workers = thread_budget(work_items, max_threads)
   def ranges = chunk_ranges(work_items, workers)
   mut queues = list(workers)
   mut i = 0
   while(i < workers){
      queues = queues.append(work_queue(i))
      i += 1
   }
   {"scheduler": work_stealing_enabled(work_items) ? "work-stealing" : "direct",
      "workers": workers, "ranges": ranges, "queues": queues,
   "status": scheduler_status(work_items)}
}

fn _serial_map(list: xs, fnptr: f): list {
   mut out = list(xs.len)
   mut i = 0
   while(i < xs.len){
      out = out.append(f(xs.get(i, 0)))
      i += 1
   }
   out
}

fn _serial_map_indexed(list: xs, fnptr: f): list {
   mut out = list(xs.len)
   mut i = 0
   while(i < xs.len){
      out = out.append(f(xs.get(i, 0), i))
      i += 1
   }
   out
}

fn _map_chunk(list: args): list {
   def f = args.get(0)
   def xs = args.get(1)
   def start = args.get(2)
   def stop = args.get(3)
   mut out = list(stop - start)
   mut i = start
   while(i < stop){
      out = out.append(f(xs.get(i, 0)))
      i += 1
   }
   out
}

fn _map_indexed_chunk(list: args): list {
   def f = args.get(0)
   def xs = args.get(1)
   def start = args.get(2)
   def stop = args.get(3)
   mut out = list(stop - start)
   mut i = start
   while(i < stop){
      out = out.append(f(xs.get(i, 0), i))
      i += 1
   }
   out
}

fn _each_chunk(list: args): int {
   def f = args.get(0)
   def xs = args.get(1)
   def start = args.get(2)
   def stop = args.get(3)
   mut i = start
   while(i < stop){
      f(xs.get(i, 0))
      i += 1
   }
   stop - start
}

fn _join_chunks(list: handles): list {
   mut out = list()
   mut i = 0
   while(i < handles.len){
      def part = thread_join(handles.get(i))
      mut j = 0
      while(j < part.len){
         out = out.append(part.get(j, 0))
         j += 1
      }
      i += 1
   }
   out
}

fn _spawn_chunks(list: xs, fnptr: f, list: ranges, fnptr: worker): list {
   mut handles = list(ranges.len)
   mut i = 0
   while(i < ranges.len){
      def r = ranges.get(i)
      handles = handles.append(thread_spawn(worker, [f, xs, r.get(0), r.get(1)]))
      i += 1
   }
   handles
}

fn parallel_map(list: xs, fnptr: f, int: max_threads=0): list {
   "Maps `f(item)` over `xs`, using worker threads when std.os.parallel policy selects them."
   def n = xs.len
   if(n == 0){ return list() }
   def workers = thread_budget(n, max_threads)
   if(workers <= 1 || !parallel_should_threads(n)){ return _serial_map(xs, f) }
   def ranges = chunk_ranges(n, workers)
   _join_chunks(_spawn_chunks(xs, f, ranges, _map_chunk))
}

fn parallel_map_indexed(list: xs, fnptr: f, int: max_threads=0): list {
   "Maps `f(item, index)` over `xs`, preserving input order."
   def n = xs.len
   if(n == 0){ return list() }
   def workers = thread_budget(n, max_threads)
   if(workers <= 1 || !parallel_should_threads(n)){ return _serial_map_indexed(xs, f) }
   def ranges = chunk_ranges(n, workers)
   _join_chunks(_spawn_chunks(xs, f, ranges, _map_indexed_chunk))
}

fn parallel_each(list: xs, fnptr: f, int: max_threads=0): int {
   "Runs `f(item)` for each item and returns the number of processed items."
   def n = xs.len
   if(n == 0){ return 0 }
   def workers = thread_budget(n, max_threads)
   if(workers <= 1 || !parallel_should_threads(n)){
      mut i = 0
      while(i < n){
         f(xs.get(i, 0))
         i += 1
      }
      return n
   }
   def ranges = chunk_ranges(n, workers)
   def handles = _spawn_chunks(xs, f, ranges, _each_chunk)
   mut done = 0
   mut i = 0
   while(i < handles.len){
      done += thread_join(handles.get(i))
      i += 1
   }
   done
}

def HARDWARE_THREADS = hardware_threads()
